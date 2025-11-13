#pragma once
#include "protocol.hpp"
#include <cstddef>

namespace WetzelMesh::Protocol
{
    // Limites de tamanho
    static constexpr size_t kMaxPacketSize = 2048;        // Tamanho máximo de pacote UART
    static constexpr size_t kMaxESPNOWPayload = 250;      // Tamanho máximo payload ESP-NOW
    static constexpr size_t kMaxBLEPayload = 512;         // Tamanho máximo payload BLE
    static constexpr size_t kMaxBodySize = 1000000;       // 1MB máximo de body
    
    // Valida tamanho do pacote antes de enviar
    bool validate_packet_size(const Packet &pkt, size_t max_size);
    
    // Valida se o pacote está completo e válido
    bool validate_packet(const Packet &pkt);

} // namespace WetzelMesh::Protocol

