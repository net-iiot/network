#include "chunk_manager.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>

namespace WetzelMesh
{
    static const char *TAG = "CHUNK_MGR";

    ChunkManager &ChunkManager::instance()
    {
        static ChunkManager inst;
        return inst;
    }

    bool ChunkManager::add_chunk(const Protocol::Packet &chunk, Protocol::Packet &reconstructed)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!chunk.is_chunk)
        {
            // Não é chunk, retorna como está
            reconstructed = chunk;
            return true;
        }
        
        uint32_t chunk_id = chunk.chunk_id;
        uint32_t chunk_total = chunk.chunk_total;
        uint32_t chunk_index = chunk.chunk_index;
        
        // Busca ou cria ChunkSet
        auto &chunk_set = chunk_sets_[chunk_id];
        chunk_set.chunk_id = chunk_id;
        chunk_set.chunk_total = chunk_total;
        chunk_set.timestamp_ms = esp_timer_get_time() / 1000ULL;
        
        // Adiciona chunk
        chunk_set.chunks[chunk_index] = chunk;
        
        ESP_LOGI(TAG, "Chunk %u/%u adicionado (id=%u, total=%u chunks)", 
                 chunk_index + 1, chunk_total, chunk_id, (unsigned)chunk_set.chunks.size());
        
        // Verifica se temos todos os chunks
        if (chunk_set.chunks.size() >= chunk_total)
        {
            // Extrai apenas os valores (packets) do map para um vector
            std::vector<Protocol::Packet> chunks_vec;
            chunks_vec.reserve(chunk_set.chunks.size());
            for (const auto &pair : chunk_set.chunks)
            {
                chunks_vec.push_back(pair.second);
            }
            
            // Reconstrói mensagem
            if (Protocol::reconstruct_from_chunks(chunks_vec, reconstructed))
            {
                ESP_LOGI(TAG, "Mensagem reconstruída: %u bytes", (unsigned)reconstructed.body.size());
                
                // Remove chunk set
                chunk_sets_.erase(chunk_id);
                return true;
            }
            else
            {
                ESP_LOGE(TAG, "Falha ao reconstruir mensagem do chunk_id=%u", chunk_id);
                chunk_sets_.erase(chunk_id);
                return false;
            }
        }
        
        return false; // Ainda faltam chunks
    }

    void ChunkManager::cleanup_old_chunks(uint64_t timeout_ms)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t now = esp_timer_get_time() / 1000ULL;
        
        for (auto it = chunk_sets_.begin(); it != chunk_sets_.end();)
        {
            if ((now - it->second.timestamp_ms) > timeout_ms)
            {
                ESP_LOGW(TAG, "Removendo chunk set expirado: id=%u", it->first);
                it = chunk_sets_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

} // namespace WetzelMesh

