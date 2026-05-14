# Server Setup Guide

This guide walks you through setting up the complete server-side stack for the ESP32 Zephyr Mesh Network vital signs monitoring system.

## Stack Overview

```
ESP32 Nodes → MQTT (Mosquitto) → Node-RED → InfluxDB → Streamlit Dashboard
```

| Component  | Role                          | Port |
|------------|-------------------------------|------|
| Mosquitto  | MQTT broker                   | 1883 |
| Node-RED   | Data routing and flow control | 1880 |
| InfluxDB   | Time-series database          | 8086 |
| Streamlit  | Visualization dashboard       | 8501 |

---

## Prerequisites

- Docker and Docker Compose installed on your PC
- Python 3.9+ (for Streamlit)
- Your PC's local IP address (e.g., `192.168.1.15`) — this is what the ESP32 root node connects to

To find your IP on Linux/macOS:
```bash
ip addr show   # or: hostname -I
```

On Windows:
```
ipconfig
```

---

## Part 1 — Docker Services (Mosquitto, Node-RED, InfluxDB)

### 1.1 Create the directory structure

```bash
mkdir -p mesh-server/mosquitto/{config,data,log}
mkdir -p mesh-server/node-red/data
mkdir -p mesh-server/influxdb/{data,config}
cd mesh-server
```

### 1.2 Configure Mosquitto

Create `mosquitto/mosquitto.conf`:

```
listener 1883
allow_anonymous true

listener 9001
protocol websockets

persistence true
persistence_location /mosquitto/data/

log_dest file /mosquitto/log/mosquitto.log
log_dest stdout
```

> **Note:** `allow_anonymous true` is fine for a local lab network. For production or shared environments, configure username/password authentication.

### 1.3 Place the docker-compose.yml

Copy `docker-compose.yml` into the `mesh-server` directory. The default credentials used in the file are listed below — **change them before deploying**:

| Variable                          | Default Value         |
|-----------------------------------|-----------------------|
| `DOCKER_INFLUXDB_INIT_USERNAME`   | `admin12345`          |
| `DOCKER_INFLUXDB_INIT_PASSWORD`   | `admin12345`          |
| `DOCKER_INFLUXDB_INIT_ORG`        | `wifi_mesh`           |
| `DOCKER_INFLUXDB_INIT_BUCKET`     | `sensor_data_bucket`  |
| `DOCKER_INFLUXDB_INIT_ADMIN_TOKEN`| `super_secret_token`  |

Replace `super_secret_token` with a strong random token. You can generate one with:
```bash
openssl rand -base64 32
```

### 1.4 Start the services

```bash
docker compose up -d
```

Verify all containers are running:
```bash
docker compose ps
```

Expected output:
```
NAME               STATUS
influxdb_tsdb      Up (healthy)
mosquitto_broker   Up
node_red_flow      Up
```

---

## Part 2 — InfluxDB Initial Setup

InfluxDB initializes automatically on first boot using the environment variables in `docker-compose.yml`. After startup, verify it is healthy:

```bash
curl http://localhost:8086/health
```

Expected response:
```json
{"name":"influxdb","message":"ready for queries and writes","status":"pass",...}
```

### 2.1 Retrieve your API token

The token defined in `DOCKER_INFLUXDB_INIT_ADMIN_TOKEN` is what Node-RED and Streamlit use. Keep it accessible — you will need it in Part 3 and Part 4.

If you need to generate a new token via the UI:

1. Open `http://localhost:8086` in your browser
2. Log in with your credentials (`admin12345` / `admin12345` by default)
3. Go to **Load Data → API Tokens → Generate API Token → All Access Token**
4. Copy and save the token

---

## Part 3 — Node-RED Flow Setup

### 3.1 Access Node-RED

Open `http://localhost:1880` in your browser.

### 3.2 Install required nodes

Go to ☰ → **Manage palette** → **Install** tab, then install:

- `node-red-contrib-influxdb` — for writing to InfluxDB

The MQTT nodes are pre-installed with Node-RED.

### 3.3 Flow overview

The actual flow used in this project has two pipelines:

**Sensor data pipeline** (top half):
```
mesh/sensor/data (MQTT in)  ┐
Test Node 2 (inject)        ├──► Format for InfluxDB (function) ──► sensor_data_bucket (InfluxDB out)
Test Node 4 (inject)        ┘                                   └──► debug
```

**Network test pipeline** (bottom half):
```
Test Results MQTT (MQTT in)   ┐
                              ├──► Test Data Debug
Simulate Test Result (inject) ┤
                              └──► Write Test Results (InfluxDB out)
```

The inject nodes (`Test Node 2`, `Test Node 4`, `Simulate Test Result`) are convenience nodes for manually triggering test payloads during development. They are not required in normal operation.

### 3.4 The "Format for InfluxDB" function node

This function node handles JSON parsing, timestamp injection, and payload formatting before writing to InfluxDB. Create a **function** node with the following code:

```javascript
// Check if the payload is a string (in case MQTT didn't parse it automatically)
if (typeof msg.payload === "string") {
    try {
        msg.payload = JSON.parse(msg.payload);
    } catch (e) {
        node.warn("Payload is not valid JSON");
        return null; // Stop processing if it's not JSON
    }
}

var options = {
    timeZone: "Asia/Manila",
    year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit',
    hour12: false
};

// Append a local timestamp to the payload (e.g., 16/12/2025, 18:20:30)
msg.payload.timestamp = new Date().toLocaleString("en-ZA", options);

return msg;
```

> **Note:** The `en-ZA` locale with `hour12: false` produces the `DD/MM/YYYY, HH:MM:SS` format. This timestamp is stored as a string field for readability. InfluxDB uses its own server-side timestamp for time-series indexing.

### 3.5 Configure the MQTT broker node

Double-click the **mesh/sensor/data** MQTT in node and set:

- **Server**: `localhost` (or the IP of your Docker host)
- **Port**: `1883`
- **Topic**: `mesh/sensor/data`
- **QoS**: `1`
- **Output**: `a parsed JSON object`

Repeat for the **Test Results MQTT** node with topic `mesh/test/results`.

### 3.6 Configure the InfluxDB output nodes

Double-click the **sensor_data_bucket** node and configure:

- **Version**: 2.0
- **URL**: `http://influxdb:8086` (Docker internal hostname; use your PC's LAN IP if Node-RED is running outside Docker)
- **Token**: your InfluxDB API token
- **Organization**: `wifi_mesh`
- **Bucket**: `sensor_data_bucket`
- **Measurement**: `max30102`

Double-click the **Write Test Results** node and use the same connection settings, but set:

- **Measurement**: `mesh_test`

### 3.7 Deploy

Click the **Deploy** button (top right). Both MQTT nodes should show a green **connected** indicator beneath them once the Mosquitto broker is reachable.

---

## Part 4 — Streamlit Dashboard

### 4.1 Install dependencies

```bash
pip install streamlit pandas plotly influxdb-client numpy
```

### 4.2 Configure app.py

Open `app.py` and update the configuration block at the top:

```python
INFLUX_URL   = "http://<YOUR_PC_IP>:8086"   # e.g., http://192.168.1.15:8086
INFLUX_TOKEN = "<your-api-token>"
INFLUX_ORG   = "wifi_mesh"
INFLUX_BUCKET = "sensor_data_bucket"
```

Replace `<YOUR_PC_IP>` with your machine's actual local IP address, and `<your-api-token>` with the token from Part 2.

> **Important:** The timezone is set to `Asia/Manila` in `app.py`. If you are in a different timezone, update the `LOCAL_TZ` variable accordingly.

### 4.3 Run the dashboard

```bash
streamlit run app.py
```

The dashboard will be available at `http://localhost:8501`.

### 4.4 Dashboard modes

The Streamlit app has two modes selectable from the sidebar:

**Patient Monitor** — displays live heart rate and SpO2 readings per node, with time-series trend charts and CSV export.

**Network Performance Test** — shows packet delivery ratio (PDR) and latency analysis by hop count. Requires test data from the `mesh/test/results` MQTT topic. A sample data demo mode is available in the sidebar for previewing the layout without live data.

---

## Part 5 — ESP32 Firmware Configuration

Update `src/mesh_config.h` with your server details:

```c
#define CONFIG_MQTT_BROKER_HOSTNAME  "192.168.1.15"  // Your PC's IP
#define CONFIG_MQTT_BROKER_PORT      1883
#define CONFIG_MQTT_PUB_TOPIC        "mesh/sensor/data"
```

The root node (Node 1) is the only node that connects to the MQTT broker. Child nodes communicate only within the mesh.

---

## Verification

### Check MQTT is receiving data

```bash
mosquitto_sub -h localhost -t "mesh/sensor/data" -v
```

### Publish a test message manually

```bash
mosquitto_pub -h localhost -t "mesh/sensor/data" \
  -m '{"node_id":2,"heart_rate":72,"spo2":98.5}'
```

### Verify data landed in InfluxDB

Open the InfluxDB UI at `http://localhost:8086`, go to **Data Explorer**, and run:

```flux
from(bucket: "sensor_data_bucket")
  |> range(start: -1h)
  |> filter(fn: (r) => r["_measurement"] == "max30102")
```

---

## Troubleshooting

### Mosquitto not accepting connections
- Check the config file path in `docker-compose.yml` matches what you created
- Ensure port 1883 is not blocked by your firewall
- On Linux: `sudo ufw allow 1883`

### Node-RED cannot reach InfluxDB
- Use `http://influxdb:8086` (Docker service name) as the URL inside Node-RED, not `localhost`
- Confirm the InfluxDB container shows `healthy` via `docker compose ps`

### Streamlit shows "Error connecting to InfluxDB"
- Confirm `INFLUX_URL` uses your machine's LAN IP, not `localhost` (unless running on the same machine without Docker)
- Verify the token matches exactly what is configured in InfluxDB
- Check InfluxDB is reachable: `curl http://<YOUR_PC_IP>:8086/health`

### Root node not connecting to MQTT broker
- Verify the IP address in `mesh_config.h` matches the machine running Docker
- Ensure the root node is connected to the router and can reach your PC
- Check the serial output of Node 1 for connection error messages

### No data in the Network Performance Test dashboard
- Enable test mode in firmware: `#define TEST_MODE_ENABLED 1`
- Confirm Node-RED has the `mesh/test/results` flow deployed
- Subscribe to the topic to verify data is arriving: `mosquitto_sub -h localhost -t "mesh/test/results" -v`
- Use the **Use Sample Data (Demo)** checkbox in the sidebar to verify the dashboard UI is working independently of data

---

## Stopping the Services

```bash
cd mesh-server
docker compose down
```

To also remove all stored data volumes:
```bash
docker compose down -v
```
