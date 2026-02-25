#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <stdint.h>
#include "protocol.hpp"

namespace NetworkMesh
{

    struct Neighbor
    {
        std::string id;
        int rssi;
        uint64_t last_seen_ms;
    };

    using PacketHandler = std::function<void(const Protocol::Packet &)>;

    class NetworkManager
    {
    public:
        static void init(bool isGateway);
        static bool is_gateway();
        static std::vector<Neighbor> neighbors();
        static bool send(const Protocol::Packet &p);
        static void handle_incoming(const Protocol::Packet &packet);
        static void route_packet(const Protocol::Packet &pkt);
        static void start_hello_task();
        static void on_hello(const Protocol::Packet &hello_packet, int rssi);
        static uint64_t now_ms();

        static PacketHandler &packet_handler();

        static std::string get_network_id();
        static const uint8_t *get_pmk();
        static uint8_t get_wifi_channel();

    private:
        static void refresh_neighbors_task(void *param);
        static void derive_pmk_from_network_id();
        static bool s_gateway;
        static std::vector<Neighbor> s_neighbors;
        static std::mutex s_neighbors_mutex;
        static std::string s_network_id;
        static uint8_t s_pmk[16];
        static PacketHandler s_packet_handler;
    };

}
