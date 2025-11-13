#pragma once
#include "protocol.hpp"
#include <map>
#include <vector>
#include <memory>
#include <mutex>

namespace WetzelMesh
{
    // Gerenciador de chunks para reconstruir mensagens grandes
    class ChunkManager
    {
    public:
        static ChunkManager &instance();
        
        // Adiciona um chunk e retorna true se a mensagem completa foi reconstruída
        bool add_chunk(const Protocol::Packet &chunk, Protocol::Packet &reconstructed);
        
        // Limpa chunks antigos (timeout)
        void cleanup_old_chunks(uint64_t timeout_ms = 60000); // 1 minuto default
        
    private:
        struct ChunkSet
        {
            uint32_t chunk_id;
            uint32_t chunk_total;
            std::map<uint32_t, Protocol::Packet> chunks; // chunk_index -> Packet
            uint64_t timestamp_ms;
        };
        
        std::map<uint32_t, ChunkSet> chunk_sets_; // chunk_id -> ChunkSet
        std::mutex mutex_;
        
        ChunkManager() = default;
    };

} // namespace WetzelMesh

