#pragma once
#include <functional>
#include <string>
#include "protocol.hpp"

namespace NetworkMesh
{

    class BorderUart
    {
    public:
        using RxHandler = std::function<void(const Protocol::Packet &)>;

        static bool init();
        static bool is_enabled();
        static void set_rx_handler(RxHandler cb);
        static bool send_to_gateway(const Protocol::Packet &pkt);
        static bool do_handshake(unsigned timeout_ms);
        static void uart_listen_task(void *);
        static bool uart_write_json(const std::string &json);
    };

}
