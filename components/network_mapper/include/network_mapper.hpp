#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdint.h>
#include "protocol.hpp"

namespace NetworkMesh
{
    struct NodeMappingInfo
    {
        std::string node_id;
        std::string node_name;
        std::string node_type;
        std::vector<std::string> capabilities;
        std::vector<Protocol::NeighborInfo> neighbors;
        bool has_gateway = false;
        std::string gateway_id;
        int rssi = 0;
        uint64_t discovered_at_ms = 0;
        float position_x = 0.0f;
        float position_y = 0.0f;
    };

    struct NetworkMap
    {
        uint64_t mapped_at_ms = 0;
        std::vector<NodeMappingInfo> nodes;
        Protocol::ConnectivityMatrix connectivity;
    };

    class NetworkMapper
    {
    public:
        static void init(bool isGateway);
        static void trigger_mapping();
        static void set_periodic_mapping(uint32_t interval_seconds);
        static void set_periodic_map_send(uint32_t interval_seconds);
        static NetworkMap get_last_map();
        static void on_discovery_response(const Protocol::Packet &response);
        static bool is_mapping_in_progress();

    private:
        static void mapping_task(void *param);
        static void send_discovery_packet();
        static void build_connectivity_matrix();
        static void send_map_to_server(const NetworkMap &map);

        static bool s_isGateway;
        static bool s_mapping_in_progress;
        static uint32_t s_periodic_interval_seconds;
        static uint32_t s_periodic_map_send_interval_seconds;
        static NetworkMap s_last_map;
        static std::map<std::string, NodeMappingInfo> s_collected_nodes;
        static uint64_t s_mapping_start_time;
        static uint32_t s_mapping_timeout_ms;
    };
}
