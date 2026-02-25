#pragma once
#include "protocol.hpp"
#include <cstddef>

namespace NetworkMesh::Protocol
{
    static constexpr size_t kMaxPacketSize = 2048;
    static constexpr size_t kMaxESPNOWPayload = 250;
    static constexpr size_t kMaxBLEPayload = 512;
    static constexpr size_t kMaxBodySize = 1000000;

    bool validate_packet_size(const Packet &pkt, size_t max_size);
    bool validate_packet(const Packet &pkt);

}
