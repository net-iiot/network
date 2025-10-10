#pragma once
#include <stdint.h>

namespace WetzelMesh
{
    class LedManager
    {
    public:
        static void init(bool isGateway);
        static void blink();

        static void set_node_joined(bool joined);
        static void set_gateway_server_connected(bool connected);
        static void on_packet_received();
    };

}
