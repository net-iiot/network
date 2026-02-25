#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace NetworkMesh::Protocol
{
    enum class PacketType
    {
        REQUEST,
        RESPONSE,
        EVENT,
        UNKNOWN
    };

    struct RouteInfo
    {
        std::string src;
        std::string dst;
    };

    struct NeighborInfo
    {
        std::string node_id;
        int rssi = 0;
        uint64_t last_seen_ms = 0;
    };

    struct HopInfo
    {
        std::string node_id;
        std::string node_name;
        uint64_t timestamp_ms;
        int rssi = 0;
        std::string transport;
    };

    struct TraceInfo
    {
        std::vector<std::string> path;
        uint32_t hop_count = 0;
        uint64_t created_at_ms = 0;
        uint64_t received_at_ms = 0;
        std::string packet_id;
        std::vector<HopInfo> hop_history;
    };

    struct Connection
    {
        std::string from_node_id;
        std::string to_node_id;
        int rssi = 0;
        uint64_t last_communication_ms = 0;
        uint32_t packet_count = 0;
        bool is_direct = true;
    };

    struct ConnectivityMatrix
    {
        std::vector<Connection> connections;
    };

    struct NodeInfo
    {
        std::string node_id;
        std::string node_name;
        std::string node_type;
        std::vector<std::string> capabilities;
        uint64_t first_seen_ms = 0;
        uint64_t last_seen_ms = 0;
        int battery_level = -1;
        float position_x = 0.0f;
        float position_y = 0.0f;
        std::map<std::string, std::string> metadata;
    };

    struct TopologyInfo
    {
        std::string node_id;
        std::string node_name;
        std::string network_id;
        std::vector<NeighborInfo> neighbors;
        bool has_gateway = false;
        std::string gateway_id;
        ConnectivityMatrix connectivity;
        NodeInfo node_info;
    };

    struct Packet
    {
        PacketType type;
        RouteInfo route;
        std::string method;
        std::string endpoint;
        int status = 0;
        std::string body;

        bool is_chunk = false;
        uint32_t chunk_id = 0;
        uint32_t chunk_total = 0;
        uint32_t chunk_index = 0;

        std::string request_id;
        TopologyInfo topology;
        TraceInfo trace;

        std::string next_hop;
        std::string routing_strategy;
        uint32_t ttl = 50;
    };

    bool parse(const std::string &jsonStr, Packet &outPacket);
    std::string serialize(const Packet &packet);

    Packet make_request(const std::string &src, const std::string &dst,
                        const std::string &method, const std::string &endpoint,
                        const std::string &body);

    Packet make_response(const std::string &src, const std::string &dst,
                         int status, const std::string &body);

    std::vector<Packet> create_chunks(const Packet &original, size_t max_chunk_size);
    bool reconstruct_from_chunks(const std::vector<Packet> &chunks, Packet &out);

    std::string generate_request_id();
    std::string generate_packet_id();

    class ChunkManager
    {
    public:
        static ChunkManager &instance();
        bool add_chunk(const Protocol::Packet &chunk, Protocol::Packet &reconstructed);
        void cleanup_old_chunks(uint64_t timeout_ms = 60000);

    private:
        struct ChunkSet
        {
            uint32_t chunk_id;
            uint32_t chunk_total;
            std::map<uint32_t, Protocol::Packet> chunks;
            uint64_t timestamp_ms;
        };

        std::map<uint32_t, ChunkSet> chunk_sets_;
        std::mutex mutex_;
        ChunkManager() = default;
    };

}
