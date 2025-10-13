#pragma once
#include <string>
#include <vector>
#include "protocol.hpp"

namespace WetzelMesh
{
    struct Neighbor
    {
        std::string id; // node-id
        int rssi = 0;
    };

    class NetworkManager
    {
    public:
        static void init(bool isGateway);

        // Envio alto nível (decide BLE ou UART)
        static bool send(const Protocol::Packet &packet);

        // Entrada da pilha quando chega algo (BLE/UART)
        static void handle_incoming(const Protocol::Packet &packet);

        static const std::vector<Neighbor> &neighbors();

        static bool is_gateway();

    private:
        static void refresh_neighbors_task(void *param);

        static bool s_gateway;
        static std::vector<Neighbor> s_neighbors;
    };
} // namespace WetzelMesh
