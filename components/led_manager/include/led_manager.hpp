#pragma once
#include <stdint.h>

namespace NetworkMesh
{
    enum class TrafficSource
    {
        MESH,
        UART,
        SERVER
    };

    class LedManager
    {
    public:
        static void init(bool isGateway);
        static void blink(TrafficSource source);
        static void set_node_joined(bool joined);
        static void set_gateway_server_connected(bool connected);
        static void set_uart_enabled(bool enabled);
        static void set_gateway_uart_connected(bool connected);
        static bool get_gateway_uart_connected();
        static void set_led_on_for_duration(uint32_t duration_ms);
    };

}
