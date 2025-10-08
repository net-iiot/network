#pragma once
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{
    class Router
    {
    public:
        static void init();
        static void handle_packet(const Protocol::Packet &packet);
        static void send_to(const Protocol::Packet &packet, const std::string &target);
    };
} // namespace WetzelMesh
