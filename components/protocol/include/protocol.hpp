#pragma once
#include <string>
#include <vector>
#include <map>

namespace WetzelMesh::Protocol
{
    enum class PacketType
    {
        REQUEST,
        RESPONSE,
        EVENT,
        UNKNOWN
    };

    struct RouteInfo
    {
        std::string src; // origem (ex: "node-01")
        std::string dst; // destino (ex: "gateway", "flutter", "node-03")
    };

    // Informação de vizinho para topologia da rede
    struct NeighborInfo
    {
        std::string node_id;  // ID do nó vizinho
        int rssi = 0;         // Força do sinal
        uint64_t last_seen_ms = 0; // Última vez que foi visto
    };

    // Informações de topologia da rede
    struct TopologyInfo
    {
        std::string node_id;                    // ID deste nó
        std::vector<NeighborInfo> neighbors;     // Lista de vizinhos conectados
        bool has_gateway = false;               // Se tem gateway conectado via UART
        std::string gateway_id;                 // ID do gateway (se houver)
    };

    struct Packet
    {
        PacketType type;      // tipo do pacote
        RouteInfo route;      // origem/destino
        std::string method;   // "GET"/"POST" (quando REQUEST)
        std::string endpoint; // ex: "/telemetry"
        int status = 0;       // status estilo HTTP (200, 404...) em RESPONSE
        std::string body;     // JSON (string)
        
        // Sistema de chunks para mensagens grandes
        bool is_chunk = false;        // Se este pacote é um chunk
        uint32_t chunk_id = 0;        // ID único do conjunto de chunks
        uint32_t chunk_total = 0;     // Total de chunks
        uint32_t chunk_index = 0;     // Índice deste chunk (0-based)
        
        // Rastreamento de requisição
        std::string request_id;        // ID único da requisição (para correlacionar request/response)
        
        // Topologia da rede (para visualização)
        TopologyInfo topology;         // Informações de topologia do nó que enviou
    };

    // Constrói um Packet a partir de string JSON
    bool parse(const std::string &jsonStr, Packet &outPacket);

    // Serializa um Packet para string JSON
    std::string serialize(const Packet &packet);

    // Helpers
    Packet make_request(const std::string &src, const std::string &dst,
                        const std::string &method, const std::string &endpoint,
                        const std::string &body);

    Packet make_response(const std::string &src, const std::string &dst,
                         int status, const std::string &body);

    // Helper para criar chunks de uma mensagem grande
    std::vector<Packet> create_chunks(const Packet &original, size_t max_chunk_size);
    
    // Helper para reconstruir chunks em uma mensagem completa
    bool reconstruct_from_chunks(const std::vector<Packet> &chunks, Packet &out);
    
    // Helper para gerar request_id único
    std::string generate_request_id();

} // namespace WetzelMesh::Protocol
