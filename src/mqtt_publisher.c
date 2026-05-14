/*
 * MQTT Publisher for Mesh Root Node
 * Publishes sensor data received from mesh child nodes to MQTT broker
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/random/random.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_publisher.h"

LOG_MODULE_REGISTER(mqtt_publisher, LOG_LEVEL_INF);

/* MQTT Broker Configuration - Update these for your setup */
#ifndef CONFIG_MQTT_BROKER_HOSTNAME
#define CONFIG_MQTT_BROKER_HOSTNAME "192.168.2.100"  /* Your PC's IP address */
#endif

#ifndef CONFIG_MQTT_BROKER_PORT
#define CONFIG_MQTT_BROKER_PORT 1883
#endif

#ifndef CONFIG_MQTT_PUB_TOPIC
#define CONFIG_MQTT_PUB_TOPIC "mesh/sensor/data"
#endif

/* Buffer sizes */
#define MQTT_RX_BUFFER_SIZE     256
#define MQTT_TX_BUFFER_SIZE     256
#define MQTT_PAYLOAD_BUFFER_SIZE 256

/* MQTT client instance */
static struct mqtt_client client;

/* Buffers for MQTT */
static uint8_t rx_buffer[MQTT_RX_BUFFER_SIZE];
static uint8_t tx_buffer[MQTT_TX_BUFFER_SIZE];
static uint8_t payload_buffer[MQTT_PAYLOAD_BUFFER_SIZE];

/* Broker address storage */
static struct sockaddr_storage broker_addr;

/* Socket poll descriptor */
static struct zsock_pollfd fds[1];
static int nfds;

/* Connection state */
static bool mqtt_connected = false;

/* Client ID buffer */
static char client_id[32];

/* Message ID counter */
static uint16_t msg_id_counter = 1;

/* ========================================================================== */
/* Internal Helper Functions                                                  */
/* ========================================================================== */

static void prepare_fds(void)
{
    fds[0].fd = client.transport.tcp.sock;
    fds[0].events = ZSOCK_POLLIN;
    nfds = 1;
}

static void clear_fds(void)
{
    nfds = 0;
}

static int poll_mqtt_socket(int timeout_ms)
{
    if (nfds <= 0) {
        return -EINVAL;
    }

    int rc = zsock_poll(fds, nfds, timeout_ms);
    if (rc < 0) {
        LOG_ERR("Socket poll error: %d", errno);
        return -errno;
    }
    return rc;
}

/* ========================================================================== */
/* MQTT Event Handler                                                         */
/* ========================================================================== */

static void mqtt_event_handler(struct mqtt_client *const cli,
                               const struct mqtt_evt *evt)
{
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result != 0) {
            LOG_ERR("MQTT CONNACK error: %d", evt->result);
            mqtt_connected = false;
        } else {
            LOG_INF("MQTT Connected to broker!");
            mqtt_connected = true;
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_INF("MQTT Disconnected");
        mqtt_connected = false;
        clear_fds();
        break;

    case MQTT_EVT_PINGRESP:
        LOG_DBG("MQTT PINGRESP received");
        break;

    case MQTT_EVT_PUBACK:
        if (evt->result != 0) {
            LOG_ERR("MQTT PUBACK error: %d", evt->result);
        } else {
            LOG_DBG("MQTT PUBACK for msg ID: %u", evt->param.puback.message_id);
        }
        break;

    default:
        LOG_DBG("MQTT event type: %d", evt->type);
        break;
    }
}

/* ========================================================================== */
/* Public API Functions                                                       */
/* ========================================================================== */

int mqtt_publisher_init(void)
{
    struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker_addr;
    int rc;

    LOG_INF("Initializing MQTT publisher...");
    LOG_INF("Broker: %s:%d", CONFIG_MQTT_BROKER_HOSTNAME, CONFIG_MQTT_BROKER_PORT);
    LOG_INF("Topic: %s", CONFIG_MQTT_PUB_TOPIC);

    /* Resolve broker address */
    memset(&broker_addr, 0, sizeof(broker_addr));
    broker4->sin_family = AF_INET;
    broker4->sin_port = htons(CONFIG_MQTT_BROKER_PORT);
    
    rc = inet_pton(AF_INET, CONFIG_MQTT_BROKER_HOSTNAME, &broker4->sin_addr);
    if (rc != 1) {
        LOG_ERR("Invalid broker IP address: %s", CONFIG_MQTT_BROKER_HOSTNAME);
        return -EINVAL;
    }

    /* Generate unique client ID */
    snprintf(client_id, sizeof(client_id), "esp32_mesh_root_%04x", 
             (uint16_t)sys_rand32_get());
    LOG_INF("MQTT Client ID: %s", client_id);

    /* Initialize MQTT client */
    mqtt_client_init(&client);

    client.broker = &broker_addr;
    client.evt_cb = mqtt_event_handler;
    client.client_id.utf8 = (uint8_t *)client_id;
    client.client_id.size = strlen(client_id);
    client.password = NULL;
    client.user_name = NULL;
    client.protocol_version = MQTT_VERSION_3_1_1;

    /* Configure buffers */
    client.rx_buf = rx_buffer;
    client.rx_buf_size = sizeof(rx_buffer);
    client.tx_buf = tx_buffer;
    client.tx_buf_size = sizeof(tx_buffer);

    /* Use non-secure transport */
    client.transport.type = MQTT_TRANSPORT_NON_SECURE;

    LOG_INF("MQTT publisher initialized");
    return 0;
}

int mqtt_publisher_connect(void)
{
    int rc;
    int64_t start_time;

    if (mqtt_connected) {
        LOG_WRN("Already connected to MQTT broker");
        return 0;
    }

    LOG_INF("Connecting to MQTT broker...");

    rc = mqtt_connect(&client);
    if (rc != 0) {
        LOG_ERR("mqtt_connect failed: %d", rc);
        return rc;
    }

    prepare_fds();

    /* Wait for CONNACK with timeout */
    start_time = k_uptime_get();
    while (!mqtt_connected) {
        rc = poll_mqtt_socket(MQTT_POLL_TIMEOUT_MS);
        if (rc > 0) {
            mqtt_input(&client);
        }

        if (k_uptime_get() - start_time > MQTT_CONNECT_TIMEOUT_MS) {
            LOG_ERR("MQTT connection timeout");
            mqtt_abort(&client);
            return -ETIMEDOUT;
        }
    }

    LOG_INF("MQTT connection established");
    return 0;
}

int mqtt_publisher_process(void)
{
    int rc;

    if (!mqtt_connected) {
        return -ENOTCONN;
    }

    rc = poll_mqtt_socket(mqtt_keepalive_time_left(&client));
    if (rc > 0) {
        if (fds[0].revents & ZSOCK_POLLIN) {
            rc = mqtt_input(&client);
            if (rc != 0) {
                LOG_ERR("mqtt_input error: %d", rc);
                return rc;
            }
        }
        if (fds[0].revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
            LOG_ERR("MQTT socket error/closed");
            mqtt_connected = false;
            return -ENOTCONN;
        }
    } else if (rc == 0) {
        /* Poll timeout - send keepalive */
        rc = mqtt_live(&client);
        if (rc != 0) {
            LOG_ERR("mqtt_live error: %d", rc);
            return rc;
        }
    }

    return 0;
}

int mqtt_publisher_send_sensor_data(uint8_t node_id, uint8_t heart_rate, float spo2)
{
    int rc;
    struct mqtt_publish_param param;
    int payload_len;

    if (!mqtt_connected) {
        LOG_WRN("MQTT not connected, cannot publish");
        return -ENOTCONN;
    }

    /* Format JSON payload */
    payload_len = snprintf((char *)payload_buffer, sizeof(payload_buffer),
                           "{\"node_id\":%d,\"heart_rate\":%d,\"spo2\":%.1f}",
                           node_id, heart_rate, (double)spo2);

    if (payload_len < 0 || payload_len >= sizeof(payload_buffer)) {
        LOG_ERR("Payload formatting error");
        return -ENOMEM;
    }

    /* Configure publish parameters */
    param.message.topic.topic.utf8 = (uint8_t *)CONFIG_MQTT_PUB_TOPIC;
    param.message.topic.topic.size = strlen(CONFIG_MQTT_PUB_TOPIC);
    param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
    param.message.payload.data = payload_buffer;
    param.message.payload.len = payload_len;
    param.message_id = msg_id_counter++;
    param.dup_flag = 0;
    param.retain_flag = 0;

    LOG_INF("Publishing: %s", payload_buffer);

    rc = mqtt_publish(&client, &param);
    if (rc != 0) {
        LOG_ERR("mqtt_publish failed: %d", rc);
        return rc;
    }

    LOG_INF("Published sensor data from Node %d (msg_id=%d)", 
            node_id, param.message_id);
    return 0;
}

bool mqtt_publisher_is_connected(void)
{
    return mqtt_connected;
}

void mqtt_publisher_disconnect(void)
{
    if (mqtt_connected) {
        mqtt_disconnect(&client);
        mqtt_connected = false;
        clear_fds();
        LOG_INF("MQTT disconnected");
    }
}
