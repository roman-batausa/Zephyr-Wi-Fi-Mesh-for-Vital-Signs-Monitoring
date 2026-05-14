#include "zephyr_mesh.h"

#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/socket.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(ZEPHYR_MESH);

#define NET_EVENT_WIFI_MASK                                                 \
    (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |     \
    NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |    \
    NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED |  \
    NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE)

// --- Module Static Variables ---

// -- Wi-Fi State --
static struct net_if *ap_iface;
static struct net_if *sta_iface;
static struct net_mgmt_event_callback cb;
static struct k_work_delayable rescan_work;
static struct k_work_delayable start_ap_work;
static bool is_root_node = false;
static bool is_connected_to_parent = false;
static bool parent_found_in_scan = false;
static char node_ap_ssid[32];
static char target_parent_ssid[32];
static char target_parent_psk[64];
static bool is_ap_started = false;
static struct net_mgmt_event_callback mgmt_cb;
static char dhcp_address[NET_IPV4_ADDR_LEN];
static char dhcp_gateway[NET_IPV4_ADDR_LEN];

// -- Topology and Routing --
static const mesh_topology_t *topology_map_ptr = NULL;
static int topology_map_size = 0;
static route_entry_t routing_table[MAX_ROUTING_ENTRIES];
static int routing_table_size = 0;
static child_node_t child_nodes[MAX_CHILD_NODES];
static int child_count = 0;

// -- Message reception flag --
static bool message_received_flag = false;

// -- Registration State --
static bool is_registered_with_parent = false;
static struct k_work_delayable registration_work;

// -- Clock Synchronization State --
static bool clock_is_synced = false;
static int64_t clock_offset_ms = 0;  // Offset to add to local time to get mesh time
static struct k_work_delayable clock_sync_work;

// -- Data/Socket State --
static int client_sock = -1;
static int server_sock = -1;
static struct sockaddr_in parent_addr;
static struct k_thread server_thread_data;
static k_tid_t server_tid;
K_THREAD_STACK_DEFINE(server_stack_area, 2048);

// -- Sensor Data Callback --
static sensor_data_callback_t sensor_callback = NULL;


// --- Forward Declarations ---
static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint32_t mgmt_event, struct net_if *iface);
static int start_wifi_scan(void);
static void server_thread(void *arg1, void *arg2, void *arg3);
static void _on_parent_connect(void);
static void _on_parent_disconnect(void);
static int start_mesh_ap(void);
static void build_routing_table(void);
static route_entry_t* find_route(uint8_t dest_node_id);
static child_node_t* find_child_by_id(uint8_t node_id);
static void add_or_update_child(uint8_t node_id, struct in_addr ip_addr);
static void handle_clock_request(struct mesh_packet *packet, struct sockaddr_in *sender_addr);
static void handle_clock_response(struct mesh_packet *packet);
static void clock_sync_work_handler(struct k_work *work);
static void send_registration_request(void);
static void handle_registration_request(struct mesh_packet *packet, struct sockaddr_in *sender_addr);
static void handle_registration_ack(struct mesh_packet *packet);
static void registration_work_handler(struct k_work *work);

// --- Clock Synchronization Functions ---

static int64_t get_local_time_ms(void)
{
    return k_uptime_get();
}

int64_t zephyr_mesh_get_time_ms(void)
{
    if (is_root_node) {
        // Root node's local time IS the mesh time
        return get_local_time_ms();
    } else {
        // Child nodes apply the offset
        return get_local_time_ms() + clock_offset_ms;
    }
}

bool zephyr_mesh_is_clock_synced(void)
{
    if (is_root_node) {
        return true;  // Root is always synced (it's the reference)
    }
    return clock_is_synced;
}

static void handle_clock_request(struct mesh_packet *packet, struct sockaddr_in *sender_addr)
{
    // Parent receives clock request from child
    LOG_INF("Received clock sync request from Node %d", packet->source_id);
    
    // Prepare response with current mesh time
    struct mesh_packet response;
    memset(&response, 0, sizeof(response));
    response.type = MESH_PACKET_CLOCK_RESP;
    response.target_id = packet->source_id;
    response.source_id = NODE_ID;
    response.ttl = 1;
    response.clock_sync.mesh_time_ms = zephyr_mesh_get_time_ms();
    response.clock_sync.request_time_ms = packet->clock_sync.request_time_ms;
    
    // Send response back to child's server port (MESH_PORT), not the ephemeral port
    // FIXED: The child listens on MESH_PORT, but sends requests from client_sock (ephemeral port)
    struct sockaddr_in child_addr;
    child_addr.sin_family = AF_INET;
    child_addr.sin_port = htons(MESH_PORT);  // Send to the child's listening port
    child_addr.sin_addr = sender_addr->sin_addr;  // Same IP as sender
    
    ssize_t sent = sendto(server_sock, &response, sizeof(response), 0, (struct sockaddr *)&child_addr, sizeof(child_addr));
    if (sent < 0) {
        LOG_ERR("Failed to send clock sync response: %d", -errno);
    } else {
        LOG_INF("Sent clock sync response to %s:%d, mesh_time=%lld ms", 
                inet_ntoa(child_addr.sin_addr), MESH_PORT, response.clock_sync.mesh_time_ms);
    }
}

static void handle_clock_response(struct mesh_packet *packet)
{
    int64_t now = get_local_time_ms();
    int64_t request_time = packet->clock_sync.request_time_ms;
    int64_t parent_mesh_time = packet->clock_sync.mesh_time_ms;
    
    // Calculate round-trip time
    int64_t rtt = now - request_time;
    
    // Estimate parent's time at the moment we receive the response
    // (parent's time + half the round trip time)
    int64_t estimated_mesh_time_now = parent_mesh_time + (rtt / 2);
    
    // Calculate offset: offset = mesh_time - local_time
    clock_offset_ms = estimated_mesh_time_now - now;
    clock_is_synced = true;
    
    LOG_INF("Clock synchronized!");
    LOG_INF("  RTT: %lld ms", rtt);
    LOG_INF("  Parent mesh time: %lld ms", parent_mesh_time);
    LOG_INF("  Clock offset: %lld ms", clock_offset_ms);
    LOG_INF("  Current mesh time: %lld ms", zephyr_mesh_get_time_ms());
}

static void clock_sync_work_handler(struct k_work *work)
{
    if (!clock_is_synced && is_registered_with_parent && is_connected_to_parent && !is_root_node) {
        // Only request clock sync after registered
        struct mesh_packet packet;
        memset(&packet, 0, sizeof(packet));
        packet.type = MESH_PACKET_CLOCK_REQ;
        packet.target_id = 0;
        packet.source_id = NODE_ID;
        packet.ttl = 1;
        packet.clock_sync.request_time_ms = get_local_time_ms();
        packet.clock_sync.mesh_time_ms = 0;
        
        LOG_INF("Sending clock sync request to parent...");
        
        ssize_t sent = sendto(client_sock, 
                              &packet, 
                              sizeof(packet), 
                              0, 
                              (struct sockaddr *)&parent_addr, 
                              sizeof(parent_addr));
        if (sent < 0) {
            LOG_ERR("Failed to send clock sync request: %d", -errno);
        }
        
        // Schedule retry
        k_work_schedule(&clock_sync_work, K_MSEC(CLOCK_SYNC_RETRY_MS));
    }
}

// --- Registration Protocol Functions ---

static void send_registration_request(void)
{
    if (is_root_node) {
        // Root does not need to register
        is_registered_with_parent = true;
        return;
    }
    
    if (!is_connected_to_parent || client_sock < 0) {
        LOG_WRN("Cannot send registration request - not connected to parent");
        return;
    }
    
    struct mesh_packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.type = MESH_PACKET_REGISTER;
    packet.target_id = 0;  // Direct to parent (not routed)
    packet.source_id = NODE_ID;
    packet.ttl = 1;
    packet.registration.node_id = NODE_ID;
    packet.registration.mesh_time_ms = 0;
    
    LOG_INF("Sending registration request to parent (Node ID: %d)...", NODE_ID);
    
    ssize_t sent = sendto(client_sock, 
                          &packet, 
                          sizeof(packet), 
                          0, 
                          (struct sockaddr *)&parent_addr, 
                          sizeof(parent_addr));
    if (sent < 0) {
        LOG_ERR("Failed to send registration request: %d", -errno);
    }
}

static void handle_registration_request(struct mesh_packet *packet, struct sockaddr_in *sender_addr)
{
    uint8_t child_node_id = packet->registration.node_id;
    
    LOG_INF("Received registration request from Node %d (IP: %s)", child_node_id, inet_ntoa(sender_addr->sin_addr));
    
    // Register the child node
    add_or_update_child(child_node_id, sender_addr->sin_addr);
    
    // Prepare registration acknowledgment with current mesh time
    struct mesh_packet response;
    memset(&response, 0, sizeof(response));
    response.type = MESH_PACKET_REGISTER_ACK;
    response.target_id = child_node_id;
    response.source_id = NODE_ID;
    response.ttl = 1;
    response.registration.node_id = NODE_ID;  // Parent's node ID
    response.registration.mesh_time_ms = zephyr_mesh_get_time_ms();
    
    // Send response to child's server port (MESH_PORT)
    struct sockaddr_in child_addr;
    child_addr.sin_family = AF_INET;
    child_addr.sin_port = htons(MESH_PORT);
    child_addr.sin_addr = sender_addr->sin_addr;
    
    ssize_t sent = sendto(server_sock, 
                          &response, 
                          sizeof(response), 
                          0, 
                          (struct sockaddr *)&child_addr, 
                          sizeof(child_addr));
    if (sent < 0) {
        LOG_ERR("Failed to send registration ACK: %d", -errno);
    } else {
        LOG_INF("Sent registration ACK to Node %d with mesh_time=%lld ms", child_node_id, response.registration.mesh_time_ms);
    }
}

static void handle_registration_ack(struct mesh_packet *packet)
{
    int64_t now = get_local_time_ms();
    int64_t parent_mesh_time = packet->registration.mesh_time_ms;
    
    LOG_INF("Received registration ACK from parent Node %d", packet->source_id);
    
    // Mark as registered
    is_registered_with_parent = true;
    
    // Use the mesh time from registration for initial clock sync
    // This is a rough sync - the clock sync protocol will refine it
    clock_offset_ms = parent_mesh_time - now;
    clock_is_synced = true;
    
    LOG_INF("Registered with parent!");
    LOG_INF("  Parent Node ID: %d", packet->source_id);
    LOG_INF("  Initial clock offset: %lld ms", clock_offset_ms);
    LOG_INF("  Current mesh time: %lld ms", zephyr_mesh_get_time_ms());
    
    // Optionally start refined clock sync after registration
    // k_work_schedule(&clock_sync_work, K_MSEC(CLOCK_SYNC_RETRY_MS));
}

static void registration_work_handler(struct k_work *work)
{
    if (!is_registered_with_parent && is_connected_to_parent && !is_root_node) {
        send_registration_request();
        // Schedule retry
        k_work_schedule(&registration_work, K_MSEC(CLOCK_SYNC_RETRY_MS));
    }
}

// --- Routing Table Functions ---

static int find_parent_of_node(uint8_t node_id)
{
    for (int i = 0; i < topology_map_size; i++) {
        if (topology_map_ptr[i].node_id == node_id) {
            return topology_map_ptr[i].parent_id;
        }
    }
    return -1;
}

static bool is_descendant_of(uint8_t potential_descendant, uint8_t ancestor)
{
    // Check if potential_descendant is a child or grandchild of ancestor
    uint8_t current = potential_descendant;
    
    while (current != 0) {
        int parent = find_parent_of_node(current);
        if (parent == -1) break;
        if (parent == ancestor) return true;
        current = parent;
    }
    return false;
}

static void build_routing_table(void)
{
    routing_table_size = 0;
    
    LOG_INF("Building routing table for Node %d...", NODE_ID);
    
    int my_parent = find_parent_of_node(NODE_ID);
    
    // For each node in the topology
    for (int i = 0; i < topology_map_size; i++) {
        uint8_t dest_id = topology_map_ptr[i].node_id;
        
        if (dest_id == NODE_ID) {
            continue; // Skip ourselves
        }
        
        bool via_parent = false;
        uint8_t next_hop = 0;
        
        // Check if destination is a descendant (child, grandchild, etc.)
        if (is_descendant_of(dest_id, NODE_ID)) {
            // Route goes down - find which direct child to use
            via_parent = false;
            uint8_t current = dest_id;
            while (true) {
                int parent = find_parent_of_node(current);
                if (parent == NODE_ID) {
                    // current is our direct child
                    next_hop = current;
                    break;
                }
                current = parent;
            }
        } else {
            // Route goes up through parent
            via_parent = true;
            next_hop = my_parent;
        }
        
        // Add to routing table
        if (routing_table_size < MAX_ROUTING_ENTRIES && next_hop != 0) {
            routing_table[routing_table_size].dest_node_id = dest_id;
            routing_table[routing_table_size].next_hop_node_id = next_hop;
            routing_table[routing_table_size].is_via_parent = via_parent;
            
            LOG_INF("Route: Node %d -> %s %d (to reach %d)", 
                    NODE_ID, 
                    via_parent ? "parent" : "child",
                    next_hop, 
                    dest_id);
            
            routing_table_size++;
        }
    }
    
    LOG_INF("Routing table built with %d entries.", routing_table_size);
}

static route_entry_t* find_route(uint8_t dest_node_id)
{
    for (int i = 0; i < routing_table_size; i++) {
        if (routing_table[i].dest_node_id == dest_node_id) {
            return &routing_table[i];
        }
    }
    return NULL;
}

// --- Child Node Tracking ---

static void add_or_update_child(uint8_t node_id, struct in_addr ip_addr)
{
    // Check if already exists
    for (int i = 0; i < child_count; i++) {
        if (child_nodes[i].node_id == node_id) {
            child_nodes[i].ip_addr = ip_addr;
            child_nodes[i].is_active = true;
            LOG_INF("Updated child Node %d IP: %s", node_id, inet_ntoa(ip_addr));
            return;
        }
    }
    
    // Add new child
    if (child_count < MAX_CHILD_NODES) {
        child_nodes[child_count].node_id = node_id;
        child_nodes[child_count].ip_addr = ip_addr;
        child_nodes[child_count].is_active = true;
        LOG_INF("Added child Node %d IP: %s", node_id, inet_ntoa(ip_addr));
        child_count++;
    } else {
        LOG_WRN("Child nodes table full!");
    }
}

static child_node_t* find_child_by_id(uint8_t node_id)
{
    for (int i = 0; i < child_count; i++) {
        if (child_nodes[i].node_id == node_id && child_nodes[i].is_active) {
            return &child_nodes[i];
        }
    }
    return NULL;
}

// --- DHCP Address Retrieval ---

static void start_dhcpv4_client(struct net_if *iface, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_INF("Start on %s: index=%d", net_if_get_device(iface)->name, net_if_get_by_iface(iface));
	net_dhcpv4_start(iface);
}

static void dhcp_event_handler(struct net_mgmt_event_callback *cb, uint32_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
        return;
    }

    // Find the DHCP-assigned address
    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type != NET_ADDR_DHCP) {
            continue;
        }

        // Get the assigned IP address
        net_addr_ntop(AF_INET,
                      &iface->config.ip.ipv4->unicast[i].ipv4.address.in_addr,
                      dhcp_address, sizeof(dhcp_address));

        // Get the gateway address
        net_addr_ntop(AF_INET,
                      &iface->config.ip.ipv4->gw,
                      dhcp_gateway, sizeof(dhcp_gateway));

        LOG_INF("DHCP: Address assigned: %s", dhcp_address);
        LOG_INF("DHCP: Gateway: %s", dhcp_gateway);
        LOG_INF("DHCP: Lease time: %u seconds", iface->config.dhcpv4.lease_time);
    }
}

// --- Internal Wi-Fi Functions ---

static void start_ap_work_handler(struct k_work *work)
{
    if (!is_ap_started) {
        start_mesh_ap();
        is_ap_started = true;
    }
}

static void rescan_work_handler(struct k_work *work)
{
	LOG_INF("Retry timer expired. Starting a new scan.");
	start_wifi_scan();
}

static int start_wifi_scan(void)
{
	LOG_INF("Starting Wi-Fi scan...");
	if (net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0)) {
		LOG_ERR("Scan request failed");
		return -1;
	}
	return 0;
}

static void enable_dhcpv4_server(void)
{
	struct in_addr addr;
	struct in_addr netmask_addr;

	if (net_addr_pton(AF_INET, WIFI_AP_IP_ADDRESS, &addr)) {
		LOG_ERR("Invalid address: %s", WIFI_AP_IP_ADDRESS);
		return;
	}

	if (net_addr_pton(AF_INET, WIFI_AP_NETMASK, &netmask_addr)) {
		LOG_ERR("Invalid netmask: %s", WIFI_AP_NETMASK);
		return;
	}

	net_if_ipv4_set_gw(ap_iface, &addr);

	if (net_if_ipv4_addr_add(ap_iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("Unable to set IP address for AP interface");
	}

	if (!net_if_ipv4_set_netmask_by_addr(ap_iface, &addr, &netmask_addr)) {
		LOG_ERR("Unable to set netmask for AP interface: %s", WIFI_AP_NETMASK);
	}

	// addr.s4_addr[3] += 6 + (NODE_ID*4);
    addr.s4_addr[3] += 10;
	if (net_dhcpv4_server_start(ap_iface, &addr) != 0) {
		LOG_ERR("DHCP server is not started for desired IP");
		return;
	}
	LOG_INF("DHCPv4 server started.");
}

static int start_mesh_ap(void)
{
	if (!ap_iface) {
		LOG_INF("AP: is not initialized");
		return -EIO;
	}
    struct wifi_connect_req_params ap_config;
	LOG_INF("Turning on AP Mode with SSID: %s", node_ap_ssid);
	ap_config.ssid = (const uint8_t *)node_ap_ssid;
	ap_config.ssid_length = strlen(node_ap_ssid);
	ap_config.psk = (const uint8_t *)WIFI_AP_PSK;
	ap_config.psk_length = strlen(WIFI_AP_PSK);
	ap_config.security = (ap_config.psk_length == 0) ? WIFI_SECURITY_TYPE_NONE : WIFI_SECURITY_TYPE_PSK;
	ap_config.channel = WIFI_CHANNEL_ANY;
	ap_config.band = WIFI_FREQ_BAND_2_4_GHZ;
	enable_dhcpv4_server();
	int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &ap_config, sizeof(struct wifi_connect_req_params));
	if (ret) {
		LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed, err: %d", ret);
	}
	return ret;
}

static int connect_to_parent(void)
{
	if (!sta_iface) {
		LOG_INF("STA: interface not initialized");
		return -EIO;
	}
    struct wifi_connect_req_params sta_config;
	sta_config.ssid = (const uint8_t *)target_parent_ssid;
	sta_config.ssid_length = strlen(target_parent_ssid);
	sta_config.psk = (const uint8_t *)target_parent_psk;
	sta_config.psk_length = strlen(target_parent_psk);
	sta_config.security = (sta_config.psk_length == 0) ? WIFI_SECURITY_TYPE_NONE : WIFI_SECURITY_TYPE_PSK;
	sta_config.channel = WIFI_CHANNEL_ANY;
	sta_config.band = WIFI_FREQ_BAND_2_4_GHZ;
	LOG_INF("Attempting to connect to parent: \"%s\"", target_parent_ssid);
	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta_config, sizeof(struct wifi_connect_req_params));
	if (ret) {
		LOG_ERR("Failed to connect to \"%s\", error: %d", target_parent_ssid, ret);
	}
	return ret;
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint32_t mgmt_event, struct net_if *iface)
{
	const struct wifi_scan_result *scan_result;
	const struct wifi_status *status;

	switch (mgmt_event) {
	case NET_EVENT_WIFI_SCAN_RESULT:
		scan_result = (const struct wifi_scan_result *)cb->info;
		if (scan_result->ssid_length == 0) { break; }
		if (strncmp(scan_result->ssid, target_parent_ssid, scan_result->ssid_length) == 0) {
			LOG_INF("Found target parent: \"%s\" [RSSI: %d]", scan_result->ssid, scan_result->rssi);
			parent_found_in_scan = true;
		}
		break;

	case NET_EVENT_WIFI_SCAN_DONE:
		LOG_INF("Scan is complete.");
		if (parent_found_in_scan) {
			LOG_INF("Parent found, proceeding to connect.");
			connect_to_parent();
		} else {
			LOG_WRN("Target parent \"%s\" not found. Retrying in %d sec...", target_parent_ssid, RETRY_DELAY_S);
			k_work_schedule(&rescan_work, K_SECONDS(RETRY_DELAY_S));
		}
		parent_found_in_scan = false;
		break;

	case NET_EVENT_WIFI_CONNECT_RESULT:
		status = (const struct wifi_status *)cb->info;
		if (status->status == 0) {
			LOG_INF("Successfully connected to parent \"%s\"!", target_parent_ssid);
            _on_parent_connect();
			if (!is_root_node && !is_ap_started) {
			    k_work_schedule(&start_ap_work, K_MSEC(100));
            }
		} else {
			LOG_ERR("Connection to \"%s\" failed. Retrying scan in %d sec...", target_parent_ssid, RETRY_DELAY_S);
			is_connected_to_parent = false;
			k_work_schedule(&rescan_work, K_SECONDS(RETRY_DELAY_S));
		}
		break;

	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("Disconnected from parent. Retrying scan in %d sec...", RETRY_DELAY_S);
		is_connected_to_parent = false;
        _on_parent_disconnect();
		k_work_schedule(&rescan_work, K_SECONDS(RETRY_DELAY_S));
		break;
	
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		LOG_INF("AP Mode enabled with SSID: %s", node_ap_ssid);
		break;
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
		LOG_INF("Child node joined.");
		break;
	default:
		break;
	}
}

// --- Internal Data/Socket Functions ---

static void server_thread(void *arg1, void *arg2, void *arg3)
{
    struct sockaddr_in server_addr;
    uint8_t rx_buf[sizeof(struct mesh_packet)];
    
    server_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_sock < 0) {
        LOG_ERR("Failed to create server socket: %d", -errno);
        return;
    }
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(MESH_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERR("Failed to bind server socket: %d", -errno);
        (void)close(server_sock);
        return;
    }
    LOG_INF("Server thread started, listening on port %d", MESH_PORT);

    while (true) {
        struct sockaddr_in sender_addr;
        socklen_t sender_addr_len = sizeof(sender_addr);
        ssize_t len = recvfrom(server_sock, 
                               rx_buf, 
                               sizeof(rx_buf), 
                               0, 
                               (struct sockaddr *)&sender_addr, 
                               &sender_addr_len);

        if (len < 0) { continue; }
        if (len != sizeof(struct mesh_packet)) { continue; }

        struct mesh_packet *packet = (struct mesh_packet *)rx_buf;

        // Handle registration packets first (they don not follow normal routing)
        if (packet->type == MESH_PACKET_REGISTER) {
            handle_registration_request(packet, &sender_addr);
            continue;
        }
        
        if (packet->type == MESH_PACKET_REGISTER_ACK) {
            handle_registration_ack(packet);
            continue;
        }

        // Handle clock sync packets (they do not follow normal routing)
        if (packet->type == MESH_PACKET_CLOCK_REQ) {
            handle_clock_request(packet, &sender_addr);
            continue;
        }
        
        if (packet->type == MESH_PACKET_CLOCK_RESP) {
            handle_clock_response(packet);
            continue;
        }

        // Handle sensor data packets
        if (packet->type == MESH_PACKET_SENSOR_DATA) {
            // Track child if it's a direct child
            int sender_parent = find_parent_of_node(packet->source_id);
            if (sender_parent == NODE_ID) {
                add_or_update_child(packet->source_id, sender_addr.sin_addr);
            }
            
            if (packet->target_id == NODE_ID) {
                // Sensor data is for us (we're the root)
                LOG_INF(">>> SENSOR DATA RECEIVED <<<");
                LOG_INF("    From: Node %d", packet->sensor.node_id);
                LOG_INF("    HR: %d bpm, SpO2: %d.%d%%",
                        packet->sensor.heart_rate_bpm,
                        packet->sensor.spo2_integer,
                        packet->sensor.spo2_decimal);
                LOG_INF("    Timestamp: %lld ms", packet->sensor.timestamp_ms);
                
                // Call the registered callback if any
                if (sensor_callback != NULL) {
                    sensor_callback(&packet->sensor);
                }
                message_received_flag = true;
            } else if (packet->ttl > 0) {
                // Forward sensor data toward root
                packet->ttl--;
                route_entry_t *route = find_route(packet->target_id);
                if (route != NULL && route->is_via_parent) {
                    if (is_connected_to_parent && client_sock >= 0) {
                        LOG_INF("Forwarding sensor data from Node %d toward root",
                                packet->sensor.node_id);
                        sendto(client_sock, packet, sizeof(*packet), 0,
                               (struct sockaddr *)&parent_addr, sizeof(parent_addr));
                    }
                }
            }
            continue;
        }

        // For DATA packets, track sender IP only if it's from a direct child
        // Check if source is our direct child in the topology
        bool is_direct_child = false;
        int sender_parent = find_parent_of_node(packet->source_id);
        if (sender_parent == NODE_ID) {
            is_direct_child = true;
            add_or_update_child(packet->source_id, sender_addr.sin_addr);
        }

        // -- Packet handling logic (DATA packets) --
        if (packet->target_id == NODE_ID) {
            LOG_INF(">>> PACKET FOR ME (Node %d) <<<", NODE_ID);
            LOG_INF("    From: Node %d", packet->source_id);
            LOG_INF("    Payload: %s", packet->payload);
            LOG_INF("    Sender IP: %s", inet_ntoa(sender_addr.sin_addr));
            
            // Set flag that a message was received
            message_received_flag = true;
        }
        else if (packet->ttl > 0) {
            packet->ttl--;
            
            // Find route for this destination
            route_entry_t *route = find_route(packet->target_id);
            
            if (route == NULL) {
                LOG_WRN("No route to Node %d from Node %d", packet->target_id, NODE_ID);
                continue;
            }
            
            LOG_INF("Forwarding packet: src=%d, dst=%d, next_hop=%d (%s)",
                    packet->source_id, 
                    packet->target_id, 
                    route->next_hop_node_id,
                    route->is_via_parent ? "parent" : "child");
            
            if (route->is_via_parent) {
                // Forward to parent
                if (is_connected_to_parent && client_sock >= 0) {
                    ssize_t sent = sendto(client_sock, 
                                          packet, 
                                          sizeof(*packet), 
                                          0, 
                                          (struct sockaddr *)&parent_addr, 
                                          sizeof(parent_addr));
                    if (sent < 0) {
                        LOG_ERR("Failed to forward to parent: %d", -errno);
                    }
                } else {
                    LOG_WRN("Cannot forward to parent - not connected");
                }
            } else {
                // Forward to specific child
                child_node_t *child = find_child_by_id(route->next_hop_node_id);
                if (child != NULL) {
                    struct sockaddr_in child_addr;
                    child_addr.sin_family = AF_INET;
                    child_addr.sin_port = htons(MESH_PORT);
                    child_addr.sin_addr = child->ip_addr;
                    
                    ssize_t sent = sendto(server_sock, packet, sizeof(*packet), 0, (struct sockaddr *)&child_addr, sizeof(child_addr));
                    if (sent < 0) {
                        LOG_ERR("Failed to forward to child: %d", -errno);
                    }
                } else {
                    LOG_WRN("Child Node %d not found in child table (may not be connected yet)", route->next_hop_node_id);
                }
            }
        } else {
            LOG_WRN("Packet TTL expired (from Node %d to Node %d)", packet->source_id, packet->target_id);
        }
    }
}

static void _on_parent_connect(void)
{
    if (client_sock >= 0) { (void)close(client_sock); }
    if (is_root_node) {
        is_connected_to_parent = true;
        is_registered_with_parent = true;  // Root doesn't need to register
        clock_is_synced = true;  // Root is always synced
        return;
    }
    client_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client_sock < 0) {
        LOG_ERR("Failed to create client socket: %d", -errno);
        return;
    }
    parent_addr.sin_family = AF_INET;
    parent_addr.sin_port = htons(MESH_PORT);
    inet_pton(AF_INET, dhcp_gateway, &parent_addr.sin_addr);
    LOG_INF("Client socket initialized. Parent: %s", dhcp_gateway);

    is_connected_to_parent = true;
    
    // Reset registration and clock sync state
    is_registered_with_parent = false;
    clock_is_synced = false;
    clock_offset_ms = 0;
    
    // Start registration process (clock sync will happen after registration)
    LOG_INF("Starting registration with parent...");
    k_work_schedule(&registration_work, K_NO_WAIT);
}

static void _on_parent_disconnect(void)
{
    if (client_sock >= 0) {
        LOG_WRN("Disconnecting client socket.");
        (void)close(client_sock);
        client_sock = -1;
    }
    
    // Mark as not registered and not synced when disconnected
    if (!is_root_node) {
        is_registered_with_parent = false;
        clock_is_synced = false;
        LOG_INF("Registration and clock sync lost due to parent disconnect.");
    }
}


// --- Public API Functions ---

void zephyr_mesh_init(void)
{
    server_tid = k_thread_create(&server_thread_data,
                    server_stack_area,
                    K_THREAD_STACK_SIZEOF(server_stack_area),
                    server_thread,
                    NULL, NULL, NULL,
                    K_PRIO_PREEMPT(8), 0, K_NO_WAIT);

	ap_iface = net_if_get_wifi_sap();
	sta_iface = net_if_get_wifi_sta();
    net_if_foreach(start_dhcpv4_client, NULL);
    net_mgmt_init_event_callback(&mgmt_cb, dhcp_event_handler, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&mgmt_cb);
	net_mgmt_init_event_callback(&cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
	net_mgmt_add_event_callback(&cb);
	k_work_init_delayable(&rescan_work, rescan_work_handler);
    k_work_init_delayable(&start_ap_work, start_ap_work_handler);
    k_work_init_delayable(&registration_work, registration_work_handler);
    k_work_init_delayable(&clock_sync_work, clock_sync_work_handler);
    LOG_INF("Zephyr Mesh Module Initialized.");
}

void zephyr_mesh_start(const mesh_topology_t *map, int map_size)
{
    // Store topology map
    topology_map_ptr = map;
    topology_map_size = map_size;
    
    // Build routing table
    build_routing_table();
    
    int parent_id = -1;
    for (int i = 0; i < map_size; i++) {
        if (map[i].node_id == NODE_ID) {
            parent_id = map[i].parent_id;
            break;
        }
    }

    snprintf(node_ap_ssid, sizeof(node_ap_ssid), "ESP32-ZephyrMesh-%02d", NODE_ID);

    if (parent_id == -1) {
        LOG_ERR("Node ID %d not in topology map! Halting.", NODE_ID);
        while (true) { k_sleep(K_SECONDS(10)); }
    } else if (parent_id == 0) {
        is_root_node = true;
        clock_is_synced = true;  // Root is always synced
        LOG_INF("Node %d configured as ROOT. Target: Router (%s)", NODE_ID, ROUTER_SSID);
        strncpy(target_parent_ssid, ROUTER_SSID, sizeof(target_parent_ssid) - 1);
        strncpy(target_parent_psk, ROUTER_PSK, sizeof(target_parent_psk) - 1);
        start_mesh_ap();
        is_ap_started = true;
    } else {
        is_root_node = false;
        LOG_INF("Node %d configured as CHILD. Target: Node %d", NODE_ID, parent_id);
        snprintf(target_parent_ssid, sizeof(target_parent_ssid), "ESP32-ZephyrMesh-%02d", parent_id);
        strncpy(target_parent_psk, WIFI_AP_PSK, sizeof(target_parent_psk) - 1);
    }
    start_wifi_scan();
}

int zephyr_mesh_send_packet(uint8_t target, const char *payload)
{
    struct mesh_packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.type = MESH_PACKET_DATA;
    packet.target_id = target;
    packet.source_id = NODE_ID;
    packet.ttl = PACKET_TTL_MAX;
    strncpy(packet.payload, payload, sizeof(packet.payload) - 1);
    packet.payload[sizeof(packet.payload) - 1] = '\0';
    
    LOG_INF("Sending packet from Node %d to Node %d", NODE_ID, target);
    
    // Find route for this destination
    route_entry_t *route = find_route(target);
    
    if (route == NULL) {
        LOG_WRN("No route to Node %d", target);
        return -EHOSTUNREACH;
    }
    
    if (route->is_via_parent) {
        // Send to parent
        if (!is_connected_to_parent || client_sock < 0) {
            LOG_WRN("Cannot send, not connected to parent.");
            return -ENOTCONN;
        }
        return sendto(client_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&parent_addr, sizeof(parent_addr));
    } else {
        // Send to child
        if (!is_ap_started || server_sock < 0) {
            LOG_WRN("Cannot send to child, AP not running.");
            return -ENETDOWN;
        }
        
        child_node_t *child = find_child_by_id(route->next_hop_node_id);
        if (child == NULL) {
            LOG_WRN("Child Node %d not found (may not be connected yet)", route->next_hop_node_id);
            return -EHOSTUNREACH;
        }
        
        struct sockaddr_in child_addr;
        child_addr.sin_family = AF_INET;
        child_addr.sin_port = htons(MESH_PORT);
        child_addr.sin_addr = child->ip_addr;
        
        LOG_INF("Sending via child Node %d (%s)", route->next_hop_node_id, 
                inet_ntoa(child->ip_addr));
        return sendto(server_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&child_addr, sizeof(child_addr));
    }
}

int zephyr_mesh_send_to_child(uint8_t target, const char *payload)
{
    if (!is_ap_started) {
        LOG_WRN("Cannot send to child, AP is not running.");
        return -ENETDOWN;
    }
    if (server_sock < 0) {
        LOG_WRN("Cannot send to child, server socket is not ready.");
        return -ENETDOWN;
    }
    
    // Find the child
    child_node_t *child = find_child_by_id(target);
    if (child == NULL) {
        LOG_WRN("Child Node %d not found", target);
        return -EHOSTUNREACH;
    }
    
    struct mesh_packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.type = MESH_PACKET_DATA;
    packet.target_id = target;
    packet.source_id = NODE_ID;
    packet.ttl = PACKET_TTL_MAX;
    strncpy(packet.payload, payload, sizeof(packet.payload) - 1);
    packet.payload[sizeof(packet.payload) - 1] = '\0';

    struct sockaddr_in child_addr;
    child_addr.sin_family = AF_INET;
    child_addr.sin_port = htons(MESH_PORT);
    child_addr.sin_addr = child->ip_addr;

    LOG_INF("Sending packet from Node %d to child Node %d (%s)", NODE_ID, target, inet_ntoa(child->ip_addr));
    return sendto(server_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&child_addr, sizeof(child_addr));
}

bool zephyr_mesh_is_root(void)
{
    return is_root_node;
}

bool zephyr_mesh_is_connected(void)
{
    return is_connected_to_parent;
}

bool zephyr_mesh_received_a_message(void)
{
    bool result = message_received_flag;
    message_received_flag = false;  // Clear flag after reading
    return result;
}

bool zephyr_mesh_is_registered(void)
{
    if (is_root_node) {
        return true;  // Root is always registered
    }
    return is_registered_with_parent;
}

// --- Sensor Data Functions ---

void zephyr_mesh_set_sensor_callback(sensor_data_callback_t callback)
{
    sensor_callback = callback;
}

int zephyr_mesh_send_sensor_data(uint8_t heart_rate_bpm, float spo2)
{
    // Target is always the root node (Node 1)
    const uint8_t ROOT_NODE_ID = 1;
    
    struct mesh_packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.type = MESH_PACKET_SENSOR_DATA;
    packet.target_id = ROOT_NODE_ID;
    packet.source_id = NODE_ID;
    packet.ttl = PACKET_TTL_MAX;
    
    // Fill sensor data
    packet.sensor.node_id = NODE_ID;
    packet.sensor.heart_rate_bpm = heart_rate_bpm;
    packet.sensor.spo2_integer = (uint8_t)spo2;
    packet.sensor.spo2_decimal = (uint8_t)((spo2 - (float)packet.sensor.spo2_integer) * 10.0f);
    packet.sensor.timestamp_ms = zephyr_mesh_get_time_ms();
    
    LOG_INF("Sending sensor data to root: HR=%d bpm, SpO2=%d.%d%%",
            packet.sensor.heart_rate_bpm,
            packet.sensor.spo2_integer,
            packet.sensor.spo2_decimal);
    
    // If we are the root, just call the callback directly
    if (is_root_node) {
        if (sensor_callback != NULL) {
            sensor_callback(&packet.sensor);
        }
        return 0;
    }
    
    // Find route to root
    route_entry_t *route = find_route(ROOT_NODE_ID);
    
    if (route == NULL) {
        LOG_WRN("No route to root node");
        return -EHOSTUNREACH;
    }
    
    if (route->is_via_parent) {
        // Send to parent
        if (!is_connected_to_parent || client_sock < 0) {
            LOG_WRN("Cannot send sensor data, not connected to parent.");
            return -ENOTCONN;
        }
        return sendto(client_sock, &packet, sizeof(packet), 0, 
                      (struct sockaddr *)&parent_addr, sizeof(parent_addr));
    } else {
        // This shouldn't happen for root-bound traffic, but handle it
        LOG_WRN("Unexpected routing for sensor data");
        return -EINVAL;
    }
}
