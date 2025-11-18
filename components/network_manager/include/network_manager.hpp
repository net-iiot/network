#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include "protocol.hpp"

namespace WetzelMesh
{

    struct Neighbor
    {
        std::string id;
        int rssi;
        uint64_t last_seen_ms;
    };

    class NetworkManager
    {
    public:
        static void init(bool isGateway);
        static bool is_gateway();
        static const std::vector<Neighbor> &neighbors();
        static bool send(const Protocol::Packet &p);
        static void handle_incoming(const Protocol::Packet &packet);
        static void start_hello_task();
        static void on_hello(const Protocol::Packet &hello_packet, int rssi);
        static uint64_t now_ms();
        
        // Network isolation
        static std::string get_network_id();
        static const uint8_t* get_pmk();  // Primary Master Key (16 bytes) para ESP-NOW encryption
        static uint8_t get_wifi_channel();

    private:
        static void refresh_neighbors_task(void *param);
        static void derive_pmk_from_network_id();
        static bool s_gateway;
        static std::vector<Neighbor> s_neighbors;
        static std::string s_network_id;
        static uint8_t s_pmk[16];  // PMK derivado do Network ID
    };

}
