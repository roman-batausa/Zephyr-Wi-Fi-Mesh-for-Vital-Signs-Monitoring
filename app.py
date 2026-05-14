import streamlit as st
import pandas as pd
import plotly.express as px
import plotly.graph_objects as go
from influxdb_client import InfluxDBClient, DeleteApi
from datetime import datetime, timedelta, timezone
import numpy as np
from zoneinfo import ZoneInfo

# Timezone configuration
LOCAL_TZ = ZoneInfo("Asia/Manila")

# --- CONFIGURATION ---
# INFLUX_URL = "http://192.168.2.100:8086"
INFLUX_URL = "http://192.168.1.15:8086"
INFLUX_TOKEN = "nOEW5spL_-rgOSeKppQPPbvFCwZN8v4tX3iOpeUD2YsW9NEcVSyiDQ9BRIQRymOkFcQGDAcFpmazFh9ZK2Nmjw=="
INFLUX_ORG = "wifi_mesh"
INFLUX_BUCKET = "sensor_data_bucket"

# Page Setup
st.set_page_config(page_title="Mesh Network Dashboard", layout="wide")

# --- MODE SELECTION ---
mode = st.sidebar.radio("Dashboard Mode", ["Patient Monitor", "Network Performance Test"])

# --- DELETE FUNCTION ---
def delete_test_data():
    """Delete all test data from InfluxDB"""
    try:
        client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
        delete_api = client.delete_api()
        
        # Delete all mesh_test measurements from the beginning of time to now
        start = datetime(1970, 1, 1, tzinfo=timezone.utc)
        stop = datetime.now(timezone.utc)
        
        delete_api.delete(
            start=start,
            stop=stop,
            predicate='_measurement="mesh_test"',
            bucket=INFLUX_BUCKET,
            org=INFLUX_ORG
        )
        client.close()
        return True
    except Exception as e:
        st.error(f"Error deleting data: {e}")
        return False

# --- DATA FETCHING FUNCTIONS ---
@st.cache_data(ttl=5)
def fetch_sensor_data(time_range):
    """Fetch sensor data from InfluxDB"""
    client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    
    query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: -{time_range})
      |> filter(fn: (r) => r["_measurement"] == "max30102")
      |> filter(fn: (r) => r["_field"] == "heart_rate" or r["_field"] == "spo2" or r["_field"] == "node_id")
      |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> keep(columns: ["_time", "node_id", "heart_rate", "spo2"])
    '''
    
    try:
        df = client.query_api().query_data_frame(query=query)
        if isinstance(df, list):
            df = pd.concat(df)
        return df
    except Exception as e:
        st.error(f"Error connecting to InfluxDB: {e}")
        return pd.DataFrame()

@st.cache_data(ttl=5)
def fetch_test_results(time_range):
    """Fetch test results from InfluxDB"""
    client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    
    # Query the same bucket used for sensor data
    query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: -{time_range})
      |> filter(fn: (r) => r["_measurement"] == "mesh_test")
      |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
    '''
    
    try:
        df = client.query_api().query_data_frame(query=query)
        if isinstance(df, list):
            df = pd.concat(df)
        return df
    except Exception as e:
        st.sidebar.error(f"InfluxDB query error: {e}")
        return pd.DataFrame()


def generate_sample_test_data():
    """Generate sample test data for demonstration"""
    np.random.seed(42)
    data = []
    
    # Generate data for different hop counts
    for run_id in range(1, 4):  # 3 test runs
        for node_id, hop_count in [(2, 1), (3, 1), (4, 2)]:
            # PDR decreases slightly with more hops
            base_pdr = 99 - (hop_count * 2)
            pdr = base_pdr + np.random.uniform(-2, 2)
            
            # Latency increases with hops
            base_latency = 15 + (hop_count * 12)
            avg_lat = base_latency + np.random.uniform(-3, 5)
            min_lat = avg_lat - np.random.uniform(5, 10)
            max_lat = avg_lat + np.random.uniform(10, 25)
            jitter = np.random.uniform(2, 8)
            
            packets_sent = 100
            packets_received = int(packets_sent * pdr / 100)
            
            data.append({
                '_time': datetime.now() - timedelta(minutes=run_id * 10),
                'node_id': node_id,
                'hop_count': hop_count,
                'test_run_id': run_id,
                'packets_sent': packets_sent,
                'packets_received': packets_received,
                'packets_lost': packets_sent - packets_received,
                'pdr_percent': round(pdr, 2),
                'avg_latency_ms': round(avg_lat, 2),
                'min_latency_ms': round(min_lat, 2),
                'max_latency_ms': round(max_lat, 2),
                'jitter_ms': round(jitter, 2)
            })
    
    return pd.DataFrame(data)


def create_pdr_chart(df):
    """Create PDR vs Hop Count chart"""
    if df.empty or 'hop_count' not in df.columns or 'pdr_percent' not in df.columns:
        return None
    
    # Group by hop count
    pdr_by_hop = df.groupby('hop_count').agg({
        'pdr_percent': ['mean', 'std', 'min', 'max', 'count']
    }).round(2)
    pdr_by_hop.columns = ['Mean PDR (%)', 'Std Dev', 'Min', 'Max', 'Samples']
    pdr_by_hop = pdr_by_hop.reset_index()
    
    fig = go.Figure()
    
    # Add bar chart with error bars
    fig.add_trace(go.Bar(
        x=pdr_by_hop['hop_count'],
        y=pdr_by_hop['Mean PDR (%)'],
        error_y=dict(type='data', array=pdr_by_hop['Std Dev']),
        name='PDR',
        marker_color=['#2ecc71', '#f39c12', '#e74c3c'][:len(pdr_by_hop)],
        text=pdr_by_hop['Mean PDR (%)'].apply(lambda x: f'{x:.1f}%'),
        textposition='outside'
    ))
    
    # Add 95% threshold line
    fig.add_hline(y=95, line_dash="dash", line_color="red", 
                  annotation_text="95% Target")
    
    fig.update_layout(
        title="Packet Delivery Ratio by Hop Count",
        xaxis_title="Hop Count",
        yaxis_title="PDR (%)",
        yaxis_range=[0, 105],
        showlegend=False
    )
    
    return fig, pdr_by_hop


def create_latency_chart(df):
    """Create Latency vs Hop Count chart"""
    if df.empty or 'hop_count' not in df.columns or 'avg_latency_ms' not in df.columns:
        return None
    
    # Group by hop count
    latency_by_hop = df.groupby('hop_count').agg({
        'avg_latency_ms': ['mean', 'std'],
        'min_latency_ms': 'min',
        'max_latency_ms': 'max',
        'jitter_ms': 'mean'
    }).round(2)
    latency_by_hop.columns = ['Avg Latency (ms)', 'Std Dev', 'Min (ms)', 'Max (ms)', 'Avg Jitter (ms)']
    latency_by_hop = latency_by_hop.reset_index()
    
    fig = go.Figure()
    
    # Add bar for average latency
    fig.add_trace(go.Bar(
        x=latency_by_hop['hop_count'],
        y=latency_by_hop['Avg Latency (ms)'],
        error_y=dict(type='data', array=latency_by_hop['Std Dev']),
        name='Avg Latency',
        marker_color=['#3498db', '#9b59b6', '#e67e22'][:len(latency_by_hop)],
        text=latency_by_hop['Avg Latency (ms)'].apply(lambda x: f'{x:.1f}ms'),
        textposition='outside'
    ))
    
    fig.update_layout(
        title="Round-Trip Latency by Hop Count",
        xaxis_title="Hop Count",
        yaxis_title="Latency (ms)",
        showlegend=False
    )
    
    return fig, latency_by_hop


def create_latency_distribution(df):
    """Create latency distribution box plot"""
    if df.empty or 'hop_count' not in df.columns:
        return None
    
    fig = go.Figure()
    
    for hop in sorted(df['hop_count'].unique()):
        hop_data = df[df['hop_count'] == hop]
        # Create synthetic distribution from summary stats
        fig.add_trace(go.Box(
            y=[hop_data['min_latency_ms'].values[0]] + 
              list(hop_data['avg_latency_ms'].values) + 
              [hop_data['max_latency_ms'].values[0]],
            name=f'{int(hop)} Hop{"s" if hop > 1 else ""}',
            boxpoints='all'
        ))
    
    fig.update_layout(
        title="Latency Distribution by Hop Count",
        yaxis_title="Latency (ms)",
        showlegend=True
    )
    
    return fig


def create_node_performance_table(df):
    """Create per-node performance summary"""
    if df.empty:
        return None
    
    summary = df.groupby(['node_id', 'hop_count']).agg({
        'pdr_percent': 'mean',
        'avg_latency_ms': 'mean',
        'min_latency_ms': 'min',
        'max_latency_ms': 'max',
        'jitter_ms': 'mean',
        'packets_sent': 'sum',
        'packets_received': 'sum'
    }).round(2).reset_index()
    
    # Rename node_id to Patient and format it
    summary['node_id'] = summary['node_id'].apply(lambda x: f"Patient {int(x)}" if pd.notna(x) else "Unknown")
    
    summary.columns = ['Patient', 'Hops', 'Avg PDR (%)', 'Avg Latency (ms)', 
                       'Min Latency (ms)', 'Max Latency (ms)', 'Avg Jitter (ms)',
                       'Total Sent', 'Total Received']
    
    return summary


def create_timeline_chart(df):
    """Create test results over time"""
    if df.empty or '_time' not in df.columns:
        return None
    
    df = df.copy()
    df['_time'] = pd.to_datetime(df['_time'])
    
    # Convert to Manila timezone
    if df['_time'].dt.tz is None:
        df['_time'] = df['_time'].dt.tz_localize('UTC')
    df['_time'] = df['_time'].dt.tz_convert(LOCAL_TZ)
    
    df['Patient'] = df['node_id'].apply(lambda x: f"Patient {int(x)}" if pd.notna(x) else "Unknown")
    
    fig = px.line(df, x='_time', y='pdr_percent', color='Patient',
                  title='PDR Over Time by Patient',
                  labels={'_time': 'Time', 'pdr_percent': 'PDR (%)'})
    
    fig.add_hline(y=95, line_dash="dash", line_color="red")
    fig.update_layout(legend_title_text="Patient")
    
    return fig


def render_patient_dashboard(time_filter):
    """Render the patient dashboard - can be called by fragment for auto-refresh"""
    # Clear cache to get fresh data when in fragment mode
    st.cache_data.clear()
    
    # Fetch data
    df = fetch_sensor_data(time_filter)
    
    if not df.empty:
        df['_time'] = pd.to_datetime(df['_time'])
        
        # Convert to Manila timezone
        if df['_time'].dt.tz is None:
            df['_time'] = df['_time'].dt.tz_localize('UTC')
        df['_time'] = df['_time'].dt.tz_convert(LOCAL_TZ)
        
        df = df.sort_values(by='_time')
        df['node_id'] = df['node_id'].astype(int).astype(str)
        
        # Create Patient column for display
        df['Patient'] = df['node_id'].apply(lambda x: f"Patient {x}")
        
        # Live Readings
        st.subheader("Live Status")
        cols = st.columns(4)
        unique_nodes = sorted(df['node_id'].unique())
        
        for i, node in enumerate(unique_nodes):
            node_df = df[df['node_id'] == node]
            
            if not node_df.empty:
                latest = node_df.iloc[-1]
                
                with cols[i % 4]:
                    st.markdown(f"### Patient {node}")
                    
                    hr = latest.get('heart_rate', 0)
                    st.metric("Heart Rate", f"{hr:.0f} BPM")
                    
                    spo2 = latest.get('spo2', 0)
                    delta_color = "normal" if spo2 >= 95 else "inverse"
                    st.metric("SpO2", f"{spo2:.1f}%", 
                              delta=f"{spo2-95:.1f}" if spo2 < 95 else "Normal", 
                              delta_color=delta_color)
                    
                    # Show last update time
                    last_time = latest.get('_time', None)
                    if last_time:
                        st.caption(f"Last: {last_time.strftime('%H:%M:%S')}")
                    st.write("---")
        
        # Charts - use Patient instead of node_id
        st.subheader("Trends")
        
        fig_hr = px.line(df, x="_time", y="heart_rate", color="Patient",
                         title="Heart Rate History", markers=True,
                         labels={"_time": "Time", "heart_rate": "Heart Rate (BPM)"})
        fig_hr.update_layout(legend_title_text="Patient")
        st.plotly_chart(fig_hr, use_container_width=True)
        
        fig_spo2 = px.line(df, x="_time", y="spo2", color="Patient",
                           title="SpO2 History", markers=True,
                           labels={"_time": "Time", "spo2": "SpO2 (%)"})
        fig_spo2.add_hline(y=95, line_dash="dash", line_color="red", 
                           annotation_text="Limit (95%)")
        fig_spo2.update_layout(legend_title_text="Patient")
        st.plotly_chart(fig_spo2, use_container_width=True)
        
        # --- EXPORT SECTION ---
        st.markdown("---")
        st.subheader("Export Data")
        
        col1, col2 = st.columns(2)
        
        with col1:
            # Prepare export dataframe with clean columns
            export_df = df[['_time', 'Patient', 'heart_rate', 'spo2']].copy()
            export_df.columns = ['Timestamp', 'Patient', 'Heart Rate (BPM)', 'SpO2 (%)']
            export_df = export_df.sort_values('Timestamp', ascending=False)
            
            csv = export_df.to_csv(index=False)
            st.download_button(
                label="Download All Data (CSV)",
                data=csv,
                file_name=f"vital_signs_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv",
                mime="text/csv"
            )
        
        with col2:
            # Summary statistics export
            summary_data = []
            for node in unique_nodes:
                node_df = df[df['node_id'] == node]
                if not node_df.empty:
                    summary_data.append({
                        'Patient': f"Patient {node}",
                        'Records': len(node_df),
                        'HR Mean (BPM)': round(node_df['heart_rate'].mean(), 1),
                        'HR Min': round(node_df['heart_rate'].min(), 1),
                        'HR Max': round(node_df['heart_rate'].max(), 1),
                        'SpO2 Mean (%)': round(node_df['spo2'].mean(), 1),
                        'SpO2 Min': round(node_df['spo2'].min(), 1),
                        'SpO2 Max': round(node_df['spo2'].max(), 1),
                        'Time Range': f"{node_df['_time'].min().strftime('%H:%M:%S')} - {node_df['_time'].max().strftime('%H:%M:%S')}"
                    })
            
            if summary_data:
                summary_df = pd.DataFrame(summary_data)
                summary_csv = summary_df.to_csv(index=False)
                st.download_button(
                    label="Download Summary (CSV)",
                    data=summary_csv,
                    file_name=f"vital_signs_summary_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv",
                    mime="text/csv"
                )
        
        # Show data preview
        with st.expander("View Raw Data"):
            display_df = export_df.copy()
            st.dataframe(display_df, use_container_width=True)
            st.caption(f"Showing {len(display_df)} records from the last {time_filter}")
        
    else:
        st.warning("No data found. Waiting for nodes to send data...")


# --- PATIENT MONITOR MODE ---
if mode == "Patient Monitor":
    st.title("Patient Monitor Dashboard")
    
    # Sidebar
    st.sidebar.header("Settings")
    time_options = ["1m", "5m", "15m", "30m", "1h", "6h", "12h", "24h"]
    time_filter = st.sidebar.selectbox("Time Range", time_options, index=2)  # Default to 15m
    
    # Auto-refresh option
    auto_refresh = st.sidebar.checkbox("Auto-refresh", value=False)
    refresh_interval = 5  # default
    if auto_refresh:
        refresh_interval = st.sidebar.selectbox("Refresh interval (seconds)", [1, 2, 5, 10], index=2)
        st.sidebar.info(f"Data refreshing every {refresh_interval}s")
    
    if st.sidebar.button("Refresh Now"):
        st.cache_data.clear()
        st.rerun()
    
    # Define the data display as a fragment that can refresh independently
    if auto_refresh:
        @st.fragment(run_every=refresh_interval)
        def display_patient_data():
            render_patient_dashboard(time_filter)
        
        display_patient_data()
    else:
        render_patient_dashboard(time_filter)


# --- NETWORK PERFORMANCE TEST MODE ---
else:
    st.title("Network Performance Test Dashboard")
    st.markdown("""
    Evaluating mesh network performance in terms of:
    - **Packet Delivery Ratio (PDR)** from leaf nodes to root
    - **End-to-end latency** across varying hop counts
    """)
    
    # Sidebar
    st.sidebar.header("Settings")
    
    # Time range options
    time_options = ["1m", "5m", "15m", "30m", "1h", "6h", "12h", "24h"]
    time_filter = st.sidebar.selectbox("Time Range", time_options, index=2)  # Default to 15m
    
    # Auto-refresh option
    auto_refresh = st.sidebar.checkbox("Auto-refresh", value=False)
    refresh_interval = 5  # default
    if auto_refresh:
        refresh_interval = st.sidebar.selectbox("Refresh interval (seconds)", [1, 2, 5, 10], index=2)
        st.sidebar.info(f"Data refreshing every {refresh_interval}s")
    
    # Data source selection
    use_sample_data = st.sidebar.checkbox("Use Sample Data (Demo)", value=False)
    
    if st.sidebar.button("Refresh Now"):
        st.cache_data.clear()
        st.rerun()
    
    # Clear data section
    st.sidebar.markdown("---")
    st.sidebar.subheader("⚠️ Data Management")
    
    if st.sidebar.button("Clear All Test Data"):
        st.session_state.show_confirm = True
    
    # Confirmation dialog
    if st.session_state.get('show_confirm', False):
        st.sidebar.warning("This will delete ALL test data from InfluxDB!")
        col1, col2 = st.sidebar.columns(2)
        with col1:
            if st.button("✓ Confirm"):
                if delete_test_data():
                    st.sidebar.success("Test data cleared!")
                    st.cache_data.clear()
                    st.session_state.show_confirm = False
                    st.rerun()
        with col2:
            if st.button("✗ Cancel"):
                st.session_state.show_confirm = False
                st.rerun()
    
    # Fetch or generate test data wrapped in fragment for auto-refresh
    def render_test_data():
        st.cache_data.clear()  # Clear cache to get fresh data
        
        if use_sample_data:
            df = generate_sample_test_data()
        else:
            df = fetch_test_results(time_filter)
        
        if not df.empty:
            # Clean up data
            if '_time' in df.columns:
                df['_time'] = pd.to_datetime(df['_time'])
                # Convert to Manila timezone
                if df['_time'].dt.tz is None:
                    df['_time'] = df['_time'].dt.tz_localize('UTC')
                df['_time'] = df['_time'].dt.tz_convert(LOCAL_TZ)
            
            # Ensure numeric columns
            numeric_cols = ['node_id', 'hop_count', 'test_run_id', 'packets_sent', 
                            'packets_received', 'packets_lost', 'pdr_percent', 
                            'avg_latency_ms', 'min_latency_ms', 'max_latency_ms', 'jitter_ms']
            for col in numeric_cols:
                if col in df.columns:
                    df[col] = pd.to_numeric(df[col], errors='coerce')
            
            # Summary metrics
            st.subheader("Summary Metrics")
            col1, col2, col3, col4 = st.columns(4)
            
            with col1:
                total_tests = len(df)
                st.metric("Total Test Runs", total_tests)
            
            with col2:
                if 'pdr_percent' in df.columns:
                    avg_pdr = df['pdr_percent'].mean()
                    st.metric("Average PDR", f"{avg_pdr:.1f}%")
            
            with col3:
                if 'avg_latency_ms' in df.columns:
                    avg_latency = df['avg_latency_ms'].mean()
                    st.metric("Average Latency", f"{avg_latency:.1f} ms")
            
            with col4:
                if 'node_id' in df.columns:
                    unique_nodes = df['node_id'].nunique()
                    st.metric("Patients Tested", unique_nodes)
            
            st.markdown("---")
            
            # Main charts
            st.subheader("PDR Analysis")
            
            col1, col2 = st.columns(2)
            
            with col1:
                result = create_pdr_chart(df)
                if result:
                    fig, pdr_table = result
                    st.plotly_chart(fig, use_container_width=True)
            
            with col2:
                result = create_latency_chart(df)
                if result:
                    fig, latency_table = result
                    st.plotly_chart(fig, use_container_width=True)
            
            st.markdown("---")
            
            # Timeline
            st.subheader("Test Timeline")
            fig = create_timeline_chart(df)
            if fig:
                st.plotly_chart(fig, use_container_width=True)
            
            st.markdown("---")
            
            # Detailed tables
            st.subheader("Detailed Results")
            
            tab1, tab2, tab3 = st.tabs(["By Hop Count", "By Patient", "Raw Data"])
            
            with tab1:
                if 'hop_count' in df.columns:
                    hop_summary = df.groupby('hop_count').agg({
                        'pdr_percent': ['mean', 'std', 'min', 'max'],
                        'avg_latency_ms': ['mean', 'std'],
                        'jitter_ms': 'mean',
                        'test_run_id': 'count'
                    }).round(2)
                    hop_summary.columns = ['PDR Mean (%)', 'PDR Std', 'PDR Min', 'PDR Max',
                                           'Latency Mean (ms)', 'Latency Std', 'Jitter (ms)', 'Test Runs']
                    st.dataframe(hop_summary, use_container_width=True)
            
            with tab2:
                node_summary = create_node_performance_table(df)
                if node_summary is not None:
                    st.dataframe(node_summary, use_container_width=True)
            
            with tab3:
                st.dataframe(df, use_container_width=True)
            
            # Export option
            st.markdown("---")
            st.subheader("Export Data")
            csv = df.to_csv(index=False)
            st.download_button(
                label="Download CSV",
                data=csv,
                file_name=f"mesh_test_results_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv",
                mime="text/csv"
            )
            
        else:
            st.warning("No test data found in InfluxDB.")
            
            st.info("**Tip:** Check the 'Use Sample Data (Demo)' checkbox in the sidebar to preview the dashboard with simulated data.")
            
            st.markdown("---")
            st.subheader("Troubleshooting")
            
            st.markdown("""
            ### Data Pipeline Check
            
            The test data flows through this pipeline:
            ```
            ESP32 Nodes → MQTT (mesh/test/results) → Node-RED → InfluxDB (mesh_test) → Streamlit
            ```
            
            **Verify each step:**
            
            1. **MQTT**: Check if messages arrive at `mesh/test/results` topic
               ```bash
               mosquitto_sub -h localhost -t "mesh/test/results" -v
               ```
            
            2. **Node-RED**: Import the flow from `nodered_test_flow.json` and configure:
               - MQTT broker connection
               - InfluxDB connection (org: `wifi_mesh`, bucket: `sensor_data_bucket`)
               - Measurement name: `mesh_test`
            
            3. **InfluxDB**: Query to verify data exists:
               ```flux
               from(bucket: "sensor_data_bucket")
                 |> range(start: -1h)
                 |> filter(fn: (r) => r["_measurement"] == "mesh_test")
               ```
            """)
            
            st.markdown("---")
            st.subheader("Node-RED Flow Setup")
            
            st.markdown("""
            Import this flow into Node-RED to bridge MQTT test results to InfluxDB:
            
            1. Open Node-RED (usually http://localhost:1880)
            2. Click hamburger menu → Import
            3. Paste the JSON below or import `nodered_test_flow.json`
            4. Configure the MQTT and InfluxDB nodes with your connection details
            5. Deploy the flow
            """)
            
            nodered_flow = '''[
        {
            "id": "test_mqtt_in",
            "type": "mqtt in",
            "name": "Test Results MQTT",
            "topic": "mesh/test/results",
            "qos": "1",
            "datatype": "json"
        },
        {
            "id": "test_influx",
            "type": "influxdb out",
            "name": "Write Test Results",
            "measurement": "mesh_test",
            "org": "wifi_mesh",
            "bucket": "sensor_data_bucket"
        }
    ]'''
            st.code(nodered_flow, language="json")
            
            st.markdown("---")
            st.subheader("Firmware Configuration")
            
            st.markdown("""
            ### Enable Test Mode in `mesh_config.h`:
            
            ```c
            #define TEST_MODE_ENABLED 1
            #define TOPOLOGY_CONFIG 0  // 0=mixed, 1=1-hop, 2=2-hop, 3=3-hop
            ```
            
            ### Configure Topology for your test:
            
            | Config | Description | Hops Tested |
            |--------|-------------|-------------|
            | 0 | Default mixed | 1 and 2 |
            | 1 | All direct to root | 1 only |
            | 2 | Through Node 2 | 1 and 2 |
            | 3 | Linear chain | 1, 2, and 3 |
            
            ### Test Parameters:
            
            | Parameter | Default | Description |
            |-----------|---------|-------------|
            | TEST_PACKET_COUNT | 100 | Packets per test run |
            | TEST_PACKET_INTERVAL_MS | 100 | Interval between packets |
            | TEST_TIMEOUT_MS | 5000 | Response timeout |
            | TEST_RUNS_PER_SESSION | 3 | Repeat count |
            """)
    
    # Run with or without auto-refresh
    if auto_refresh:
        @st.fragment(run_every=refresh_interval)
        def display_test_data():
            render_test_data()
        
        display_test_data()
    else:
        render_test_data()

# Footer
st.sidebar.markdown("---")
st.sidebar.markdown("**ESP32 Zephyr Mesh Network**")
st.sidebar.markdown("v2.1 - Patient Monitor")
st.sidebar.caption(f"Manila Time: {datetime.now(LOCAL_TZ).strftime('%H:%M:%S')}")