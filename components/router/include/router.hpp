#pragma once
#include <string>
#include "esp_log.h"
#include "protocol.hpp"

namespace WetzelMesh
{

    class Router
    {
    public:
        static void init();
        static void handle_packet(const Protocol::Packet &packet);
        static void send_to_gateway(const Protocol::Packet &packet);
        static void send_to_ble(const Protocol::Packet &packet);
        static void send_to_uart(const Protocol::Packet &packet);
    };

} // namespace WetzelMesh
