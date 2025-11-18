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

    // Informações de cada hop no caminho do pacote
    struct HopInfo
    {
        std::string node_id;
        std::string node_name;             // Nome do node (ex: "Sala 1", "Corredor")
        uint64_t timestamp_ms;
        int rssi = 0;                      // Força do sinal ao receber
        std::string transport;             // "UART", "MESH", "BLE"
    };

    // Rastreamento de rota percorrida pelo pacote
    struct TraceInfo
    {
        std::vector<std::string> path;        // Caminho percorrido: ["gateway", "border", "node-01", ...]
        uint32_t hop_count = 0;               // Número de saltos
        uint64_t created_at_ms = 0;           // Timestamp de criação (gateway)
        uint64_t received_at_ms = 0;          // Timestamp de recebimento neste node
        std::string packet_id;                 // ID único do pacote (UUID)
        std::vector<HopInfo> hop_history;     // Histórico detalhado de cada hop
    };

    // Conexão entre dois nodes
    struct Connection
    {
        std::string from_node_id;
        std::string to_node_id;
        int rssi = 0;
        uint64_t last_communication_ms = 0;
        uint32_t packet_count = 0;        // Quantos pacotes foram enviados
        bool is_direct = true;            // Conexão direta ou via relay
    };

    // Matriz de conectividade: quem pode enviar para quem
    struct ConnectivityMatrix
    {
        std::vector<Connection> connections;
    };

    // Informações completas de um node
    struct NodeInfo
    {
        std::string node_id;                  // ID técnico (ex: "node-01")
        std::string node_name;                // Nome amigável (ex: "Sala 1", "Corredor")
        std::string node_type;                // "gateway", "border", "node"
        std::vector<std::string> capabilities; // ["sensor_temp", "actuator", etc]
        uint64_t first_seen_ms = 0;           // Quando foi descoberto
        uint64_t last_seen_ms = 0;            // Última vez visto
        int battery_level = -1;               // Nível de bateria (-1 = não disponível)
        float position_x = 0.0f;              // Posição X (para visualização)
        float position_y = 0.0f;              // Posição Y (para visualização)
        std::map<std::string, std::string> metadata; // Metadados extras
    };

    // Informações de topologia da rede
    struct TopologyInfo
    {
        std::string node_id;                    // ID deste nó
        std::string node_name;                  // Nome do node (ex: "Sala 1", "Corredor")
        std::string network_id;                 // ID da rede (ex: "rede-01", "rede-02") - para isolamento
        std::vector<NeighborInfo> neighbors;     // Lista de vizinhos conectados
        bool has_gateway = false;               // Se tem gateway conectado via UART
        std::string gateway_id;                 // ID do gateway (se houver)
        ConnectivityMatrix connectivity;         // Matriz de conectividade conhecida
        NodeInfo node_info;                     // Informações completas do node
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
        
        // Rastreabilidade completa
        TraceInfo trace;               // Informações de rastreamento do pacote
        
        // Metadados de roteamento
        std::string next_hop;         // Próximo node no caminho
        std::string routing_strategy; // "flooding", "shortest_path", etc
        uint32_t ttl = 10;            // Time to live (evitar loops)
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
    
    // Helper para gerar packet_id único (UUID-like)
    std::string generate_packet_id();

} // namespace WetzelMesh::Protocol
