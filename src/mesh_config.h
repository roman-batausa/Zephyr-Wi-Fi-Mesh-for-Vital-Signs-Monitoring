#ifndef MESH_CONFIG_H
#define MESH_CONFIG_H

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

//==================================================================================
// ---  Mesh Node Configuration ---
//==================================================================================
// !! IMPORTANT !!
// !! Set this ID for each node before flashing. This defines the mesh topology. !!
#define NODE_ID 1
//==================================================================================

//==================================================================================
// ---  MQTT Broker Configuration (for Root Node) ---
//==================================================================================
// !! Set this to your PC's IP address on the WiFi network !!
#define CONFIG_MQTT_BROKER_HOSTNAME "192.168.2.100"
#define CONFIG_MQTT_BROKER_PORT     1883
#define CONFIG_MQTT_PUB_TOPIC       "mesh/sensor/data"
//==================================================================================

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

// --- Wi-Fi AP Mode Configuration ---
#define WIFI_AP_PSK         "" // "" for an open network.
#define WIFI_AP_IP_ADDRESS  "192.168."STR(NODE_ID)".1"
#define WIFI_AP_NETMASK     "255.255.255.0"
#define WIFI_AP_BROADCAST_ADDRESS "192.168."STR(NODE_ID)".255"


// --- Wi-Fi STA Mode Configuration (for the root node) ---
#define ROUTER_SSID  "RESE"      // <<<<<<< REPLACE WITH WIFI SSID
#define ROUTER_PSK   "enerconn"  // <<<<<<< REPLACE WITH WIFI PASSWORD

// --- Mesh Logic Configuration ---
#define RETRY_DELAY_S 5
#define MESH_PORT 8080
#define MAX_CHILD_NODES 5
#define PACKET_TTL_MAX 10
#define MAX_ROUTING_ENTRIES 20

// --- Clock Synchronization Configuration ---
#define CLOCK_SYNC_RETRY_MS     1000    // Retry sync every 1 second if not synced
#define CLOCK_SYNC_TIMEOUT_MS   500     // Timeout waiting for sync response

// --- Shared Data Structures ---

typedef struct {
    int node_id;
    int parent_id;
} mesh_topology_t;

// Packet types for mesh communication
typedef enum {
    MESH_PACKET_DATA = 0,       // Normal data packet
    MESH_PACKET_CLOCK_REQ = 1,  // Clock sync request (child -> parent)
    MESH_PACKET_CLOCK_RESP = 2, // Clock sync response (parent -> child)
    MESH_PACKET_REGISTER = 3,   // Registration request (child -> parent)
    MESH_PACKET_REGISTER_ACK = 4, // Registration acknowledgment (parent -> child)
    MESH_PACKET_SENSOR_DATA = 5,  // Sensor data packet (node -> root)
} mesh_packet_type_t;

// Sensor data structure for SpO2/HR readings
typedef struct {
    uint8_t node_id;        // Source node ID
    uint8_t heart_rate_bpm; // Heart rate in BPM (0-255)
    uint8_t spo2_integer;   // SpO2 integer part (0-100)
    uint8_t spo2_decimal;   // SpO2 decimal part (0-9, for one decimal place)
    int64_t timestamp_ms;   // Mesh timestamp when reading was taken
} sensor_reading_t;

struct mesh_packet {
    uint8_t type;       // mesh_packet_type_t
    uint8_t target_id;
    uint8_t source_id;
    uint8_t ttl;
    union {
        char payload[128];              // For DATA packets
        struct {
            int64_t mesh_time_ms;       // Mesh time in milliseconds
            int64_t request_time_ms;    // Original request time (for RTT calculation)
        } clock_sync;                   // For CLOCK_REQ and CLOCK_RESP packets
        struct {
            uint8_t node_id;            // Registering node's ID
            int64_t mesh_time_ms;       // For REGISTER_ACK: current mesh time
        } registration;                 // For REGISTER and REGISTER_ACK packets
        sensor_reading_t sensor;        // For SENSOR_DATA packets
    };
};

// Routing table entry
typedef struct {
    uint8_t dest_node_id;      // Destination node
    uint8_t next_hop_node_id;  // Next hop (parent or child)
    bool is_via_parent;        // true = forward to parent, false = forward to child
} route_entry_t;

// Child node tracking
typedef struct {
    uint8_t node_id;
    struct in_addr ip_addr;
    bool is_active;
} child_node_t;

#endif // MESH_CONFIG_H
