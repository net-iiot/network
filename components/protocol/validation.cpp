#include "validation.hpp"
#include "esp_log.h"

namespace NetworkMesh::Protocol
{
    static const char *TAG = "VALID";

    bool validate_packet_size(const Packet &pkt, size_t max_size)
    {
        std::string serialized = serialize(pkt);
        if (serialized.size() > max_size)
        {
            ESP_LOGW(TAG, "Pacote muito grande: %u bytes (max=%u)",
                     (unsigned)serialized.size(), (unsigned)max_size);
            return false;
        }
        return true;
    }

    bool validate_packet(const Packet &pkt)
    {
        if (pkt.route.src.empty() || pkt.route.dst.empty())
        {
            ESP_LOGE(TAG, "Pacote invalido: src ou dst vazio");
            return false;
        }

        if (pkt.type == PacketType::UNKNOWN)
        {
            ESP_LOGE(TAG, "Pacote invalido: tipo UNKNOWN");
            return false;
        }

        if (pkt.type == PacketType::REQUEST)
        {
            if (pkt.method.empty() || pkt.endpoint.empty())
            {
                ESP_LOGE(TAG, "Pacote REQUEST invalido: method ou endpoint vazio");
                return false;
            }
        }

        if (pkt.is_chunk)
        {
            if (pkt.chunk_total == 0 || pkt.chunk_index >= pkt.chunk_total)
            {
                ESP_LOGE(TAG, "Chunk invalido: total=%u index=%u",
                         pkt.chunk_total, pkt.chunk_index);
                return false;
            }
        }

        if (pkt.body.size() > kMaxBodySize)
        {
            ESP_LOGE(TAG, "Body muito grande: %u bytes (max=%u)",
                     (unsigned)pkt.body.size(), (unsigned)kMaxBodySize);
            return false;
        }

        return true;
    }

}
