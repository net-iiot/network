#pragma once
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{

    class Gateway
    {
    public:
        static void init();
        static bool send(const Protocol::Packet &pkt);
        static bool send_to_border(const Protocol::Packet &pkt);

    private:
        static bool uart_write_json(const std::string &json);
        static void uart_listen_task(void *);
    };
}
