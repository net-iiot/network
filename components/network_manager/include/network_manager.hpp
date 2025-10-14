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
        static void on_hello(const std::string &node_id, int rssi);
        static uint64_t now_ms();

    private:
        static void refresh_neighbors_task(void *param);
        static bool s_gateway;
        static std::vector<Neighbor> s_neighbors;
    };

}
