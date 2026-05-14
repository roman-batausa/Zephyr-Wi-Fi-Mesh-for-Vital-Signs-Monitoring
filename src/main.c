/*
 * ESP32 Zephyr Mesh Network with MAX30102 Sensor and MQTT Publishing
 * 
 * This firmware combines WiFi mesh networking with pulse oximeter sensor
 * readings. Stable sensor readings are transmitted to the root node, which
 * then publishes them to an MQTT broker for visualization in Grafana.
 * 
 * If sensor or display initialization fails, the mesh network continues
 * operating without those features.
 */

#include "mesh_config.h"
#include "zephyr_mesh.h"
#include "filters.h"
#include "mqtt_publisher.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>
#include <math.h>
#include <stdio.h>

LOG_MODULE_REGISTER(MAIN);

/* ========================================================================== */
/* LED Configuration                                                          */
/* ========================================================================== */

#define LED_CYCLE_PERIOD_MS  4000
#define CONN_BLINK_ON_MS     100
#define CONN_BLINK_OFF_MS    100
#define RECV_BLINK_MS        200

#define BLINK1_START_MS      0
#define BLINK1_END_MS        100
#define BLINK2_START_MS      200
#define BLINK2_END_MS        300

static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led_g), gpios);
static const struct gpio_dt_spec led_y = GPIO_DT_SPEC_GET(DT_ALIAS(led_y), gpios);
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led_r), gpios);

/* ========================================================================== */
/* Sensor Configuration                                                       */
/* ========================================================================== */

#define SAMPLING_RATE_HZ        400.0f
#define FINGER_THRESHOLD        10000
#define FINGER_COOLDOWN_MS      500
#define EDGE_THRESHOLD          (-5000.0f)
#define LOW_PASS_CUTOFF_HZ      5.0f
#define HIGH_PASS_CUTOFF_HZ     0.5f
#define MIN_BPM                 50
#define MAX_BPM                 250
#define MIN_HEARTBEAT_INTERVAL_MS   300

#define SPO2_COEFF_A            1.5958422f
#define SPO2_COEFF_B            (-34.6596622f)
#define SPO2_COEFF_C            112.6898759f

#define STABILIZATION_TIME_MS       30000
#define STABLE_HOLD_TIME_MS         5000
#define BPM_STABILITY_TOLERANCE     5
#define SPO2_STABILITY_TOLERANCE    2.0f
#define ROLLING_AVG_SIZE            10
#define DISPLAY_UPDATE_MS           250

/* ========================================================================== */
/* Topology Definition                                                        */
/* ========================================================================== */

static const mesh_topology_t topology_map[] = {
    {1, 0}, // Node 1 is root (parent is router)
    {2, 1}, // Node 2's parent is Node 1
    {3, 1}, // Node 3's parent is Node 1
    {4, 3}, // Node 4's parent is Node 3
};

/* ========================================================================== */
/* Thread Definitions                                                         */
/* ========================================================================== */

static struct k_thread led_thread_data;
static k_tid_t led_tid;
K_THREAD_STACK_DEFINE(led_stack_area, 1024);

static struct k_thread sensor_thread_data;
static k_tid_t sensor_tid;
K_THREAD_STACK_DEFINE(sensor_stack_area, 4096);

static struct k_thread mqtt_thread_data;
static k_tid_t mqtt_tid;
K_THREAD_STACK_DEFINE(mqtt_stack_area, 4096);

/* ========================================================================== */
/* Global State                                                               */
/* ========================================================================== */

/* Sensor and display initialization status */
static bool sensor_initialized = false;
static bool display_initialized = false;
static const struct device *sensor_dev = NULL;
static const struct device *display_dev = NULL;

/* Message received flash state */
static volatile bool pending_recv_flash = false;

/* Filter instances */
static struct low_pass_filter lpf_red;
static struct low_pass_filter lpf_ir;
static struct high_pass_filter hpf;
static struct differentiator diff;
static struct moving_avg_filter avg_bpm;
static struct moving_avg_filter avg_spo2;
static struct min_max_avg_stat stat_red;
static struct min_max_avg_stat stat_ir;

/* Sensor state variables */
static int64_t last_heartbeat_ms = 0;
static int64_t finger_timestamp_ms = 0;
static int64_t finger_start_time_ms = 0;
static bool finger_detected = false;
static float last_diff = NAN;
static bool crossed = false;
static int64_t crossed_time_ms = 0;

/* Stable reading tracking */
static float current_avg_bpm = 0.0f;
static float current_avg_spo2 = 0.0f;
static float stable_candidate_bpm = 0.0f;
static float stable_candidate_spo2 = 0.0f;
static int64_t stable_start_time_ms = 0;
static bool is_stabilized = false;
static bool has_stable_reading = false;
static float final_stable_bpm = 0.0f;
static float final_stable_spo2 = 0.0f;
static float highest_stable_spo2 = 0.0f;
static float bpm_at_highest_spo2 = 0.0f;
static int64_t last_display_update_ms = 0;

/* Flag to indicate stable reading has been sent */
static bool stable_reading_sent = false;

/* ========================================================================== */
/* Filter and State Functions                                                 */
/* ========================================================================== */

static void init_filters(void)
{
    lpf_init(&lpf_red, LOW_PASS_CUTOFF_HZ, SAMPLING_RATE_HZ);
    lpf_init(&lpf_ir, LOW_PASS_CUTOFF_HZ, SAMPLING_RATE_HZ);
    hpf_init(&hpf, HIGH_PASS_CUTOFF_HZ, SAMPLING_RATE_HZ);
    diff_init(&diff, SAMPLING_RATE_HZ);
    mavg_init(&avg_bpm, ROLLING_AVG_SIZE);
    mavg_init(&avg_spo2, ROLLING_AVG_SIZE);
    stat_init(&stat_red);
    stat_init(&stat_ir);
}

static void reset_all_filters(void)
{
    lpf_reset(&lpf_red);
    lpf_reset(&lpf_ir);
    hpf_reset(&hpf);
    diff_reset(&diff);
    mavg_reset(&avg_bpm);
    mavg_reset(&avg_spo2);
    stat_reset(&stat_red);
    stat_reset(&stat_ir);
    last_diff = NAN;
    crossed = false;
}

static void reset_stable_state(void)
{
    current_avg_bpm = 0.0f;
    current_avg_spo2 = 0.0f;
    stable_candidate_bpm = 0.0f;
    stable_candidate_spo2 = 0.0f;
    stable_start_time_ms = 0;
    is_stabilized = false;
    has_stable_reading = false;
    final_stable_bpm = 0.0f;
    final_stable_spo2 = 0.0f;
    highest_stable_spo2 = 0.0f;
    bpm_at_highest_spo2 = 0.0f;
    stable_reading_sent = false;
}

static inline float calculate_spo2(float r)
{
    return SPO2_COEFF_A * r * r + SPO2_COEFF_B * r + SPO2_COEFF_C;
}

static bool is_reading_stable(float bpm, float spo2, float ref_bpm, float ref_spo2)
{
    return (fabsf(bpm - ref_bpm) <= BPM_STABILITY_TOLERANCE) &&
           (fabsf(spo2 - ref_spo2) <= SPO2_STABILITY_TOLERANCE);
}

/* ========================================================================== */
/* Display Functions                                                          */
/* ========================================================================== */

static int display_init_func(void)
{
    display_dev = DEVICE_DT_GET_ANY(sinowealth_sh1106);
    if (display_dev == NULL) {
        LOG_WRN("No SH1106 display found");
        return -ENODEV;
    }
    
    if (!device_is_ready(display_dev)) {
        LOG_WRN("Display device not ready");
        display_dev = NULL;
        return -ENODEV;
    }
    
    if (display_blanking_off(display_dev) != 0) {
        LOG_WRN("Failed to turn off display blanking");
    }
    
    int ret = cfb_framebuffer_init(display_dev);
    if (ret != 0) {
        LOG_WRN("CFB init failed: %d", ret);
        display_dev = NULL;
        return ret;
    }
    
    cfb_framebuffer_clear(display_dev, true);
    cfb_framebuffer_set_font(display_dev, 0);
    
    LOG_INF("SH1106 display initialized successfully");
    return 0;
}

static void display_update(int64_t now_ms)
{
    if (display_dev == NULL) {
        return;
    }
    
    if (now_ms - last_display_update_ms < DISPLAY_UPDATE_MS) {
        return;
    }
    last_display_update_ms = now_ms;
    
    char line1[32];
    char line2[32];
    char line3[32];
    char line4[32];
    
    cfb_framebuffer_clear(display_dev, false);
    
    if (!finger_detected) {
        snprintf(line1, sizeof(line1), "Place finger");
        snprintf(line2, sizeof(line2), "on sensor...");
        snprintf(line3, sizeof(line3), "Node ID: %d", NODE_ID);
        snprintf(line4, sizeof(line4), zephyr_mesh_is_connected() ? "Mesh: OK" : "Mesh: ...");
    } else if (!is_stabilized) {
        int64_t elapsed_ms = now_ms - finger_start_time_ms;
        int remaining_sec = (STABILIZATION_TIME_MS - elapsed_ms) / 1000;
        if (remaining_sec < 0) remaining_sec = 0;
        
        snprintf(line1, sizeof(line1), "Stabilizing");
        snprintf(line2, sizeof(line2), "Wait: %d sec", remaining_sec);
        
        if (current_avg_bpm > 0) {
            snprintf(line3, sizeof(line3), "HR: %d bpm", (int)current_avg_bpm);
            snprintf(line4, sizeof(line4), "SpO2: %.1f%%", (double)current_avg_spo2);
        } else {
            snprintf(line3, sizeof(line3), "Reading...");
            snprintf(line4, sizeof(line4), "");
        }
    } else if (!has_stable_reading) {
        snprintf(line1, sizeof(line1), "Finding stable");
        snprintf(line2, sizeof(line2), "reading...");
        snprintf(line3, sizeof(line3), "HR: %d bpm", (int)current_avg_bpm);
        snprintf(line4, sizeof(line4), "SpO2: %.1f%%", (double)current_avg_spo2);
    } else {
        snprintf(line1, sizeof(line1), "=== STABLE ===");
        snprintf(line2, sizeof(line2), "HR: %d bpm", (int)bpm_at_highest_spo2);
        snprintf(line3, sizeof(line3), "SpO2: %.1f%%", (double)highest_stable_spo2);
        snprintf(line4, sizeof(line4), stable_reading_sent ? "Sent!" : "Sending...");
    }
    
    cfb_print(display_dev, line1, 0, 0);
    cfb_print(display_dev, line2, 0, 16);
    cfb_print(display_dev, line3, 0, 32);
    cfb_print(display_dev, line4, 0, 48);
    
    cfb_framebuffer_finalize(display_dev);
}

/* ========================================================================== */
/* Stable Reading Tracking and Transmission                                   */
/* ========================================================================== */

static void update_stable_tracking(float bpm, float spo2, int64_t now_ms)
{
    current_avg_bpm = bpm;
    current_avg_spo2 = spo2;
    
    /* Check if we've passed the stabilization period */
    if (!is_stabilized) {
        if (now_ms - finger_start_time_ms >= STABILIZATION_TIME_MS) {
            is_stabilized = true;
            stable_candidate_bpm = bpm;
            stable_candidate_spo2 = spo2;
            stable_start_time_ms = now_ms;
            LOG_INF("Stabilization complete, looking for stable reading...");
        }
        return;
    }
    
    /* After stabilization, track stable readings */
    if (is_reading_stable(bpm, spo2, stable_candidate_bpm, stable_candidate_spo2)) {
        /* Reading is stable, check if it's been stable long enough */
        if (now_ms - stable_start_time_ms >= STABLE_HOLD_TIME_MS) {
            /* This reading has been stable for 5 seconds */
            final_stable_bpm = (stable_candidate_bpm + bpm) / 2.0f;
            final_stable_spo2 = (stable_candidate_spo2 + spo2) / 2.0f;
            
            /* Track highest stable SpO2 */
            if (final_stable_spo2 > highest_stable_spo2) {
                highest_stable_spo2 = final_stable_spo2;
                bpm_at_highest_spo2 = final_stable_bpm;
                has_stable_reading = true;
                LOG_INF("New highest stable reading: HR=%d, SpO2=%.1f%%",
                        (int)bpm_at_highest_spo2, (double)highest_stable_spo2);
                
                /* Send stable reading to root node via mesh */
                if (!stable_reading_sent && zephyr_mesh_is_connected() && 
                    zephyr_mesh_is_clock_synced()) {
                    int ret = zephyr_mesh_send_sensor_data(
                        (uint8_t)bpm_at_highest_spo2, 
                        highest_stable_spo2);
                    if (ret >= 0) {
                        stable_reading_sent = true;
                        LOG_INF("Stable reading sent to root node!");
                    } else {
                        LOG_WRN("Failed to send stable reading: %d", ret);
                    }
                }
            }
            
            /* Reset for next stable period detection */
            stable_candidate_bpm = bpm;
            stable_candidate_spo2 = spo2;
            stable_start_time_ms = now_ms;
        }
    } else {
        /* Reading changed, reset stable candidate */
        stable_candidate_bpm = bpm;
        stable_candidate_spo2 = spo2;
        stable_start_time_ms = now_ms;
    }
}

/* ========================================================================== */
/* Sensor Processing                                                          */
/* ========================================================================== */

static void process_sample(uint32_t red_raw, uint32_t ir_raw, int64_t timestamp_ms)
{
    float current_value_red = (float)red_raw;
    float current_value_ir = (float)ir_raw;
    
    /* Finger detection */
    if (red_raw > FINGER_THRESHOLD) {
        if (timestamp_ms - finger_timestamp_ms > FINGER_COOLDOWN_MS) {
            if (!finger_detected) {
                finger_start_time_ms = timestamp_ms;
                reset_stable_state();
                LOG_INF("Finger detected, starting 30-second stabilization...");
            }
            finger_detected = true;
        }
    } else {
        if (finger_detected) {
            LOG_INF("Finger removed");
            if (has_stable_reading) {
                LOG_INF("Final stable reading: HR=%d bpm, SpO2=%.1f%%",
                        (int)bpm_at_highest_spo2, (double)highest_stable_spo2);
            }
        }
        reset_all_filters();
        reset_stable_state();
        finger_detected = false;
        finger_timestamp_ms = timestamp_ms;
        return;
    }
    
    if (!finger_detected) {
        return;
    }
    
    /* Apply low pass filters */
    current_value_red = lpf_process(&lpf_red, current_value_red);
    current_value_ir = lpf_process(&lpf_ir, current_value_ir);
    
    /* Collect statistics for SpO2 calculation */
    stat_process(&stat_red, current_value_red);
    stat_process(&stat_ir, current_value_ir);
    
    /* Heart beat detection */
    float current_value = hpf_process(&hpf, current_value_red);
    float current_diff = diff_process(&diff, current_value);
    
    if (isnan(current_diff) || isnan(last_diff)) {
        last_diff = current_diff;
        return;
    }
    
    /* Detect zero crossing */
    if (last_diff > 0 && current_diff < 0) {
        crossed = true;
        crossed_time_ms = timestamp_ms;
    }
    
    if (current_diff > 0) {
        crossed = false;
    }
    
    /* Detect heartbeat */
    if (crossed && current_diff < EDGE_THRESHOLD) {
        if (last_heartbeat_ms != 0 && 
            crossed_time_ms - last_heartbeat_ms > MIN_HEARTBEAT_INTERVAL_MS) {
            
            int bpm = 60000 / (int)(crossed_time_ms - last_heartbeat_ms);
            
            float r_red = (stat_maximum(&stat_red) - stat_minimum(&stat_red)) / 
                          stat_average(&stat_red);
            float r_ir = (stat_maximum(&stat_ir) - stat_minimum(&stat_ir)) / 
                         stat_average(&stat_ir);
            float r = r_red / r_ir;
            float spo2 = calculate_spo2(r);
            
            if (spo2 > 100.0f) spo2 = 100.0f;
            if (spo2 < 0.0f) spo2 = 0.0f;
            
            if (bpm >= MIN_BPM && bpm <= MAX_BPM) {
                float avg_bpm_val = mavg_process(&avg_bpm, (float)bpm);
                float avg_spo2_val = mavg_process(&avg_spo2, spo2);
                
                if (mavg_count(&avg_bpm) >= 3) {
                    update_stable_tracking(avg_bpm_val, avg_spo2_val, timestamp_ms);
                }
            }
            
            stat_reset(&stat_red);
            stat_reset(&stat_ir);
        }
        
        crossed = false;
        last_heartbeat_ms = crossed_time_ms;
    }
    
    last_diff = current_diff;
}

/* ========================================================================== */
/* Thread Functions                                                           */
/* ========================================================================== */

static void led_status_thread(void *arg1, void *arg2, void *arg3)
{
    LOG_INF("LED status thread started.");
    
    LOG_INF("Waiting for clock synchronization...");
    while (!zephyr_mesh_is_clock_synced()) {
        gpio_pin_set_dt(&led_y, 1);
        k_sleep(K_MSEC(500));
        gpio_pin_set_dt(&led_y, 0);
        k_sleep(K_MSEC(500));
    }
    LOG_INF("Clock synchronized!");
    
    while (1) {
        int64_t mesh_time = zephyr_mesh_get_time_ms();
        int64_t cycle_position = mesh_time % LED_CYCLE_PERIOD_MS;
        
        if (zephyr_mesh_received_a_message()) {
            pending_recv_flash = true;
        }
        
        if (pending_recv_flash) {
            gpio_pin_set_dt(&led_y, 1);
            k_sleep(K_MSEC(RECV_BLINK_MS));
            gpio_pin_set_dt(&led_y, 0);
            pending_recv_flash = false;
            continue;
        }
        
        bool connected = zephyr_mesh_is_connected();
        
        bool should_be_on = false;
        if (cycle_position >= BLINK1_START_MS && cycle_position < BLINK1_END_MS) {
            should_be_on = true;
        } else if (cycle_position >= BLINK2_START_MS && cycle_position < BLINK2_END_MS) {
            should_be_on = true;
        }
        
        gpio_pin_set_dt(&led_g, (connected && should_be_on) ? 1 : 0);
        gpio_pin_set_dt(&led_r, (!connected && should_be_on) ? 1 : 0);
        
        int64_t next_change;
        if (cycle_position < BLINK1_START_MS) {
            next_change = BLINK1_START_MS;
        } else if (cycle_position < BLINK1_END_MS) {
            next_change = BLINK1_END_MS;
        } else if (cycle_position < BLINK2_START_MS) {
            next_change = BLINK2_START_MS;
        } else if (cycle_position < BLINK2_END_MS) {
            next_change = BLINK2_END_MS;
        } else {
            next_change = LED_CYCLE_PERIOD_MS;
        }
        
        int64_t sleep_time = next_change - cycle_position;
        if (sleep_time < 10) {
            sleep_time = 10;
        }
        
        k_sleep(K_MSEC(sleep_time));
    }
}

static void sensor_thread(void *arg1, void *arg2, void *arg3)
{
    struct sensor_value red_val, ir_val;
    int ret;
    
    LOG_INF("Sensor thread started.");
    
    /* Wait for mesh to be ready */
    while (!zephyr_mesh_is_clock_synced()) {
        k_sleep(K_MSEC(100));
    }
    LOG_INF("Mesh ready, starting sensor readings.");
    
    init_filters();
    
    while (1) {
        int64_t now = k_uptime_get();
        
        ret = sensor_sample_fetch(sensor_dev);
        if (ret < 0) {
            if (display_initialized) {
                display_update(now);
            }
            k_sleep(K_MSEC(10));
            continue;
        }
        
        ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_RED, &red_val);
        if (ret < 0) {
            continue;
        }
        
        ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_IR, &ir_val);
        if (ret < 0) {
            continue;
        }
        
        process_sample((uint32_t)red_val.val1, (uint32_t)ir_val.val1, now);
        
        if (display_initialized) {
            display_update(now);
        }
        
        k_sleep(K_USEC(2500));
    }
}

/* MQTT thread - only runs on root node */
static void mqtt_thread(void *arg1, void *arg2, void *arg3)
{
    int rc;
    
    LOG_INF("MQTT thread started (root node only).");
    
    /* Wait for WiFi connection to router */
    LOG_INF("Waiting for WiFi connection to router...");
    while (!zephyr_mesh_is_connected()) {
        k_sleep(K_MSEC(500));
    }
    LOG_INF("WiFi connected to router.");
    
    /* Give network stack time to fully initialize */
    k_sleep(K_SECONDS(3));
    
    /* Initialize MQTT */
    rc = mqtt_publisher_init();
    if (rc != 0) {
        LOG_ERR("MQTT init failed: %d", rc);
        return;
    }
    
    /* Main MQTT loop */
    while (1) {
        /* Connect to broker if not connected */
        if (!mqtt_publisher_is_connected()) {
            LOG_INF("Connecting to MQTT broker...");
            rc = mqtt_publisher_connect();
            if (rc != 0) {
                LOG_WRN("MQTT connect failed, retrying in 5s...");
                k_sleep(K_SECONDS(5));
                continue;
            }
        }
        
        /* Process MQTT events */
        rc = mqtt_publisher_process();
        if (rc != 0 && rc != -ENOTCONN) {
            LOG_WRN("MQTT process error: %d", rc);
        }
        
        k_sleep(K_MSEC(100));
    }
}

/* ========================================================================== */
/* Sensor Data Callback (for root node - publishes to MQTT)                   */
/* ========================================================================== */

static void on_sensor_data_received(const sensor_reading_t *reading)
{
    float spo2 = (float)reading->spo2_integer + (float)reading->spo2_decimal / 10.0f;
    
    LOG_INF("========================================");
    LOG_INF("SENSOR DATA FROM NODE %d", reading->node_id);
    LOG_INF("  Heart Rate: %d bpm", reading->heart_rate_bpm);
    LOG_INF("  SpO2: %d.%d%%", reading->spo2_integer, reading->spo2_decimal);
    LOG_INF("========================================");
    
    /* Publish to MQTT if connected */
    if (mqtt_publisher_is_connected()) {
        int rc = mqtt_publisher_send_sensor_data(
            reading->node_id,
            reading->heart_rate_bpm,
            spo2);
        if (rc == 0) {
            LOG_INF("Sensor data published to MQTT!");
        } else {
            LOG_WRN("Failed to publish to MQTT: %d", rc);
        }
    } else {
        LOG_WRN("MQTT not connected, data not published");
    }
}

/* ========================================================================== */
/* Main Entry Point                                                           */
/* ========================================================================== */

int main(void)
{
    k_sleep(K_SECONDS(4));
    LOG_INF("=== ESP32 Zephyr Mesh Node %d ===", NODE_ID);
    
    if (NODE_ID == 1) {
        LOG_INF("*** ROOT NODE - Will publish to MQTT ***");
        LOG_INF("MQTT Broker: %s:%d", CONFIG_MQTT_BROKER_HOSTNAME, CONFIG_MQTT_BROKER_PORT);
        LOG_INF("MQTT Topic: %s", CONFIG_MQTT_PUB_TOPIC);
    }
    
    /* Initialize LEDs */
    if (!gpio_is_ready_dt(&led_g) || !gpio_is_ready_dt(&led_y) || !gpio_is_ready_dt(&led_r)) {
        LOG_ERR("LED GPIO devices not ready!");
        return -1;
    }
    
    gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_y, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
    LOG_INF("LEDs initialized.");
    
    /* Initialize display (non-fatal if fails) */
    if (display_init_func() == 0) {
        display_initialized = true;
        LOG_INF("Display initialized successfully.");
    } else {
        display_initialized = false;
        LOG_WRN("Display initialization failed - continuing without display.");
    }
    
    /* Initialize sensor (non-fatal if fails) */
    sensor_dev = DEVICE_DT_GET_ANY(maxim_max30101);
    if (sensor_dev == NULL) {
        LOG_WRN("No MAX30101/MAX30102 sensor found - continuing without sensor.");
        sensor_initialized = false;
    } else if (!device_is_ready(sensor_dev)) {
        LOG_WRN("Sensor device not ready - continuing without sensor.");
        sensor_initialized = false;
        sensor_dev = NULL;
    } else {
        sensor_initialized = true;
        LOG_INF("MAX30102 sensor initialized successfully.");
    }
    
    /* Initialize the mesh module */
    zephyr_mesh_init();
    
    /* Register sensor data callback (only useful for root node) */
    zephyr_mesh_set_sensor_callback(on_sensor_data_received);
    
    /* Configure and start the mesh */
    zephyr_mesh_start(topology_map, ARRAY_SIZE(topology_map));
    
    /* Start the LED status thread (always runs) */
    led_tid = k_thread_create(&led_thread_data,
                    led_stack_area,
                    K_THREAD_STACK_SIZEOF(led_stack_area),
                    led_status_thread,
                    NULL, NULL, NULL,
                    K_PRIO_PREEMPT(7),
                    0, K_NO_WAIT);
    
    /* Start the sensor thread only if sensor was initialized */
    if (sensor_initialized) {
        sensor_tid = k_thread_create(&sensor_thread_data,
                        sensor_stack_area,
                        K_THREAD_STACK_SIZEOF(sensor_stack_area),
                        sensor_thread,
                        NULL, NULL, NULL,
                        K_PRIO_PREEMPT(8),
                        0, K_NO_WAIT);
        LOG_INF("Sensor thread started.");
    } else {
        LOG_INF("Sensor thread NOT started (sensor not available).");
    }
    
    /* Start MQTT thread only on root node */
    if (NODE_ID == 1) {
        mqtt_tid = k_thread_create(&mqtt_thread_data,
                        mqtt_stack_area,
                        K_THREAD_STACK_SIZEOF(mqtt_stack_area),
                        mqtt_thread,
                        NULL, NULL, NULL,
                        K_PRIO_PREEMPT(9),
                        0, K_NO_WAIT);
        LOG_INF("MQTT thread started (root node).");
    }
    
    LOG_INF("Initialization complete. Mesh network running.");
    
    return 0;
}
