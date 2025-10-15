#pragma once

#include <string>
#include "protocol.hpp"  // ✅ necessário para reconhecer Protocol::Packet

namespace WetzelMesh
{
    class Gateway
    {
    public:
        static void init();
        static void uart_listen_task(void *);
        static bool send(const Protocol::Packet &pkt);
        static bool send_to_border(const Protocol::Packet &pkt);
        static bool uart_write_json(const std::string &json); // ✅ agora pública
    };
} // namespace WetzelMesh
