#ifndef ZEPHYR_MESH_H
#define ZEPHYR_MESH_H

#include "mesh_config.h"

/**
 * @brief Initialize the Zephyr Mesh module.
 *
 * This starts the server thread to listen for incoming packets
 * and initializes the Wi-Fi event handlers.
 */
void zephyr_mesh_init(void);

/**
 * @brief Configure and start the Wi-Fi and networking process.
 *
 * This will configure the node based on its ID and the map,
 * build the routing table, then start the scan/connect process.
 *
 * @param map       Pointer to the mesh topology map.
 * @param map_size  Number of entries in the map.
 */
void zephyr_mesh_start(const mesh_topology_t *map, int map_size);

/**
 * @brief Send a data packet through the mesh network.
 *
 * This function sends a packet to any node in the mesh using the
 * routing table to determine the correct next hop (parent or child).
 *
 * @param target    The destination NODE_ID.
 * @param payload   A null-terminated string to send.
 * @return 0 on success, negative error code on failure.
 */
int zephyr_mesh_send_packet(uint8_t target, const char *payload);

/**
 * @brief Send a data packet directly to a connected child node.
 *
 * This function is for nodes that have an Access Point (AP) running
 * and want to send directly to one of their children.
 *
 * @param target    The destination child NODE_ID.
 * @param payload   A null-terminated string to send.
 * @return 0 on success, negative error code on failure.
 */
int zephyr_mesh_send_to_child(uint8_t target, const char *payload);

/**
 * @brief Check if this node is configured as the root.
 * @return true if this is the root node, false otherwise.
 */
bool zephyr_mesh_is_root(void);

/**
 * @brief Check if the node is currently connected to its parent.
 * @return true if connected, false otherwise.
 */
bool zephyr_mesh_is_connected(void);

/**
 * @brief Check if the node received any message since last check.
 * This function clears the flag after reading.
 * @return true if received, false otherwise.
 */
bool zephyr_mesh_received_a_message(void);

/**
 * @brief Get the synchronized mesh time in milliseconds.
 *
 * For the root node, this returns the uptime since boot.
 * For child nodes, this returns the local time adjusted by
 * the clock offset calculated during synchronization.
 *
 * @return Synchronized mesh time in milliseconds.
 */
int64_t zephyr_mesh_get_time_ms(void);

/**
 * @brief Check if the node's clock is synchronized with the mesh.
 *
 * The root node is always considered synchronized.
 * Child nodes are synchronized after receiving a clock response
 * from their parent.
 *
 * @return true if synchronized, false otherwise.
 */
bool zephyr_mesh_is_clock_synced(void);

/**
 * @brief Check if the node is registered with its parent.
 *
 * The root node is always considered registered.
 * Child nodes are registered after receiving a registration ACK
 * from their parent.
 *
 * @return true if registered, false otherwise.
 */
bool zephyr_mesh_is_registered(void);

/**
 * @brief Send sensor data to the root node.
 *
 * This function sends a sensor reading packet to the root node (Node 1).
 * The packet will be routed through the mesh network to reach the root.
 *
 * @param heart_rate_bpm Heart rate in BPM
 * @param spo2 SpO2 percentage (e.g., 98.5)
 * @return 0 on success, negative error code on failure.
 */
int zephyr_mesh_send_sensor_data(uint8_t heart_rate_bpm, float spo2);

/**
 * @brief Callback type for receiving sensor data at root node.
 *
 * @param reading Pointer to the received sensor reading
 */
typedef void (*sensor_data_callback_t)(const sensor_reading_t *reading);

/**
 * @brief Register a callback for receiving sensor data.
 *
 * Only meaningful for the root node. When sensor data packets
 * arrive at the root, this callback will be invoked.
 *
 * @param callback Function to call when sensor data is received
 */
void zephyr_mesh_set_sensor_callback(sensor_data_callback_t callback);

#endif // ZEPHYR_MESH_H
