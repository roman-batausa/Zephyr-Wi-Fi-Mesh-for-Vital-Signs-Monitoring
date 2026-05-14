/*
 * MQTT Publisher for Mesh Root Node
 * Publishes sensor data received from mesh child nodes to MQTT broker
 */

#ifndef MQTT_PUBLISHER_H
#define MQTT_PUBLISHER_H

#include <zephyr/kernel.h>
#include <stdbool.h>

/* MQTT connection timeout in milliseconds */
#define MQTT_CONNECT_TIMEOUT_MS     5000
#define MQTT_POLL_TIMEOUT_MS        500
#define MQTT_RECONNECT_DELAY_MS     5000

/**
 * @brief Initialize the MQTT publisher module
 * 
 * Sets up the MQTT client and prepares for connection.
 * Must be called after WiFi is connected.
 * 
 * @return 0 on success, negative error code on failure
 */
int mqtt_publisher_init(void);

/**
 * @brief Connect to the MQTT broker
 * 
 * Blocks until connected or timeout.
 * 
 * @return 0 on success, negative error code on failure
 */
int mqtt_publisher_connect(void);

/**
 * @brief Process MQTT events (call periodically)
 * 
 * Handles incoming messages and keepalive.
 * Should be called in a loop or from a thread.
 * 
 * @return 0 on success, negative error code on failure
 */
int mqtt_publisher_process(void);

/**
 * @brief Publish sensor data to MQTT broker
 * 
 * @param node_id Node ID that collected the data
 * @param heart_rate Heart rate in BPM
 * @param spo2 SpO2 percentage (e.g., 98.5)
 * @return 0 on success, negative error code on failure
 */
int mqtt_publisher_send_sensor_data(uint8_t node_id, uint8_t heart_rate, float spo2);

/**
 * @brief Check if MQTT client is connected
 * 
 * @return true if connected, false otherwise
 */
bool mqtt_publisher_is_connected(void);

/**
 * @brief Disconnect from MQTT broker
 */
void mqtt_publisher_disconnect(void);

#endif /* MQTT_PUBLISHER_H */
