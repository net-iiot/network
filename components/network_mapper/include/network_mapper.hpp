#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdint.h>
#include "protocol.hpp"

namespace WetzelMesh
{
    // Informações coletadas de um node durante o mapeamento
    struct NodeMappingInfo
    {
        std::string node_id;
        std::string node_name;
        std::string node_type;
        std::vector<std::string> capabilities;
        std::vector<Protocol::NeighborInfo> neighbors;
        bool has_gateway = false;
        std::string gateway_id;
        int rssi = 0;
        uint64_t discovered_at_ms = 0;
        float position_x = 0.0f;
        float position_y = 0.0f;
    };

    // Resultado completo do mapeamento
    struct NetworkMap
    {
        uint64_t mapped_at_ms = 0;
        std::vector<NodeMappingInfo> nodes;
        Protocol::ConnectivityMatrix connectivity;
    };

    class NetworkMapper
    {
    public:
        // Inicializa o mapeador
        static void init(bool isGateway);
        
        // Dispara mapeamento imediatamente
        static void trigger_mapping();
        
        // Configura mapeamento periódico (0 = desabilitado)
        static void set_periodic_mapping(uint32_t interval_seconds);
        
        // Configura envio periódico do mapa para servidor (0 = desabilitado)
        static void set_periodic_map_send(uint32_t interval_seconds);
        
        // Obtém último mapeamento realizado
        static NetworkMap get_last_map();
        
        // Processa resposta de DISCOVERY de um node
        static void on_discovery_response(const Protocol::Packet &response);
        
        // Verifica se mapeamento está em andamento
        static bool is_mapping_in_progress();

    private:
        static void mapping_task(void *param);
        static void send_discovery_packet();
        static void build_connectivity_matrix();
        static void send_map_to_server(const NetworkMap &map);
        
        static bool s_isGateway;
        static bool s_mapping_in_progress;
        static uint32_t s_periodic_interval_seconds;
        static uint32_t s_periodic_map_send_interval_seconds;
        static NetworkMap s_last_map;
        static std::map<std::string, NodeMappingInfo> s_collected_nodes;
        static uint64_t s_mapping_start_time;
        static uint32_t s_mapping_timeout_ms;
    };
}

