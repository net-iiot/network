#pragma once
#include "protocol.hpp" 
#include "espnow_transport.hpp"
#include "led_manager.hpp"

namespace WetzelMesh {
    class Router {
    public:
        static void init(bool isGateway);
        static void handle_packet(const Protocol::Packet &pkt);
    };
}
