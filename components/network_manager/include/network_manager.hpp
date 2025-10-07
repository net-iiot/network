#pragma once
#include <string>
#include <vector>
#include "protocol.hpp"

namespace WetzelMesh
{

    struct Neighbor
    {
        std::string id;
        int rssi;
    };

    class NetworkManager
    {
    public:
        static void init(bool isGateway);
        static void broadcast(const Protocol::Packet &packet);
        static void handle_incoming(const Protocol::Packet &packet);
        static const std::vector<Neighbor> &get_neighbors();

    private:
        static std::vector<Neighbor> neighbors;
        static void scan_neighbors();
        static void on_ble_message(const std::string &json);
        static bool gatewayMode;
    };

} // namespace WetzelMesh
