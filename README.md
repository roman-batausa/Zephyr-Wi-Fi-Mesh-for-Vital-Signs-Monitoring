# ESP32 Zephyr Mesh Network with MQTT Publishing

This project combines a WiFi mesh network with MAX30102 pulse oximeter sensor readings. Child nodes measure heart rate and SpO2, transmit stable readings to the root node, which then publishes them to an MQTT broker for visualization.

## Features

- **WiFi Mesh Networking**: Tree-topology mesh network with preconfigured routing
- **Pulse Oximeter Integration**: MAX30102 sensor for heart rate and SpO2 measurement
- **Stable Reading Detection**: 30-second stabilization followed by 5-second consistency check
- **MQTT Publishing**: Root node publishes sensor data to MQTT broker
- **Graceful Degradation**: Mesh continues if sensor/display fails to initialize
- **JSON Payload**: Simple JSON format for easy integration

## Data Flow

```
Child Node (Sensor) → Mesh Network → Root Node → MQTT Broker → Node-RED → InfluxDB → Streamlit visualization app
```

## MQTT Payload Format

The root node publishes JSON messages to the configured MQTT topic:

```json
{
  "node_id": 2,
  "heart_rate": 72,
  "spo2": 98.5
}
```

## Configuration

### 1. Set Your PC's IP Address (for MQTT broker)

Edit `src/mesh_config.h`:

```c
#define CONFIG_MQTT_BROKER_HOSTNAME "192.168.1.100"  // Your PC's IP
#define CONFIG_MQTT_BROKER_PORT     1883
#define CONFIG_MQTT_PUB_TOPIC       "mesh/sensor/data"
```

### 2. Set WiFi Credentials (for root node's router connection)

```c
#define ROUTER_SSID  "YourWiFiNetwork"
#define ROUTER_PSK   "YourWiFiPassword"
```

### 3. Set Node ID

Each node needs a unique ID:

```c
#define NODE_ID 1  // 1 = root, 2-4 = child nodes
```

## Topology

Default configuration:
```
Router (WiFi)
    └── Node 1 (Root) ─── MQTT ───► Your PC
        ├── Node 2 (Sensor)
        └── Node 3 (Relay)
            └── Node 4 (Sensor)
```

## Building

### For Root Node (Node 1):
```bash
# Set NODE_ID to 1 in mesh_config.h
# Set your PC's IP in CONFIG_MQTT_BROKER_HOSTNAME
# Set WiFi credentials in ROUTER_SSID and ROUTER_PSK

west build -b esp32_devkitc_wroom//procpu
west flash
```

### For Child Nodes (Nodes 2, 3, 4):
```bash
# Set NODE_ID to appropriate value (2, 3, or 4)
# MQTT and WiFi router settings are not used on child nodes

west build -b esp32_devkitc_wroom//procpu
west flash
```

## Files

- `src/main.c` - Main application with sensor processing and MQTT integration
- `src/mqtt_publisher.c/h` - MQTT client for publishing sensor data
- `src/zephyr_mesh.c/h` - Mesh networking implementation
- `src/mesh_config.h` - Configuration and data structures
- `src/filters.h` - DSP filters for heart rate detection

## LED Indicators

- **Green**: Connected to parent/router (double-blink)
- **Red**: Not connected (double-blink)
- **Yellow**: Message received / Waiting for sync

## Server and Streamlit App Setup

See `SERVER_SETUP_GUIDE.md` for complete instructions on setting up:
- Mosquitto (MQTT broker)
- Node-RED (data routing)
- InfluxDB (time-series database)
- Streamlit (visualization)

## Testing

### Monitor MQTT from command line:
```bash
mosquitto_sub -h YOUR_PC_IP -t "mesh/sensor/data" -v
```

### Publish test data:
```bash
mosquitto_pub -h YOUR_PC_IP -t "mesh/sensor/data" -m '{"node_id":2,"heart_rate":72,"spo2":98.5}'
```

## Troubleshooting

### Root node not connecting to MQTT
1. Verify PC's IP address is correct
2. Check that Mosquitto is running
3. Ensure firewall allows port 1883
4. Check serial output for connection errors

### Sensor data not reaching Streamlit
1. Check MQTT broker is receiving messages
2. Verify Node-RED flow is deployed
3. Check InfluxDB token in Node-RED
4. Verify Streamlit data source connection
