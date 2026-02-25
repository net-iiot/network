#include "network_mapper.hpp"
#include "network_manager.hpp"
#include "gateway.hpp"
#include "border_uart.hpp"
#include "espnow_transport.hpp"
#include "ble_transport.hpp"
#include "led_manager.hpp"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>

namespace NetworkMesh
{
    static const char *TAG = "NETMAP";

    bool NetworkMapper::s_isGateway = false;
    bool NetworkMapper::s_mapping_in_progress = false;
    uint32_t NetworkMapper::s_periodic_interval_seconds = 0;
    uint32_t NetworkMapper::s_periodic_map_send_interval_seconds = 300;
    NetworkMap NetworkMapper::s_last_map;
    std::map<std::string, NodeMappingInfo> NetworkMapper::s_collected_nodes;
    uint64_t NetworkMapper::s_mapping_start_time = 0;
    uint32_t NetworkMapper::s_mapping_timeout_ms = 10000;

    void NetworkMapper::init(bool isGateway)
    {
        s_isGateway = isGateway;
        ESP_LOGI(TAG, "NetworkMapper inicializado (Gateway: %s)", isGateway ? "SIM" : "NÃO");

        if (isGateway)
        {
            xTaskCreatePinnedToCore(mapping_task, "netmap_task", 8192, nullptr, 4, nullptr, tskNO_AFFINITY);

            auto initial_mapping = [](void *) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                ESP_LOGI(TAG, "Disparando mapeamento inicial...");
                trigger_mapping();
                vTaskDelete(nullptr);
            };
            xTaskCreatePinnedToCore(initial_mapping, "init_map", 4096, nullptr, 3, nullptr, tskNO_AFFINITY);
        }
    }

    void NetworkMapper::trigger_mapping()
    {
        if (s_mapping_in_progress)
        {
            ESP_LOGW(TAG, "Mapeamento já em andamento, ignorando trigger");
            return;
        }

        if (!s_isGateway)
        {
            ESP_LOGW(TAG, "Apenas gateway pode iniciar mapeamento");
            return;
        }

        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "INICIANDO MAPEAMENTO DA REDE");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

        s_mapping_in_progress = true;
        s_collected_nodes.clear();
        s_mapping_start_time = esp_timer_get_time() / 1000ULL;

        send_discovery_packet();
    }

    void NetworkMapper::set_periodic_mapping(uint32_t interval_seconds)
    {
        s_periodic_interval_seconds = interval_seconds;
        ESP_LOGI(TAG, "Mapeamento periódico configurado: a cada %u segundos", interval_seconds);
    }

    void NetworkMapper::set_periodic_map_send(uint32_t interval_seconds)
    {
        s_periodic_map_send_interval_seconds = interval_seconds;
        ESP_LOGI(TAG, "Envio periódico do mapa configurado: a cada %u segundos", interval_seconds);
    }

    NetworkMap NetworkMapper::get_last_map()
    {
        return s_last_map;
    }

    void NetworkMapper::on_discovery_response(const Protocol::Packet &response)
    {
        if (!s_mapping_in_progress)
        {
            ESP_LOGW(TAG, "DISCOVERY_RESPONSE ignorada: mapeamento não está em andamento");
            return;
        }

        if (response.type != Protocol::PacketType::RESPONSE || response.method != "DISCOVERY_RESPONSE")
        {
            ESP_LOGW(TAG, "DISCOVERY_RESPONSE ignorada: tipo ou método incorreto (type=%d, method=%s)",
                     (int)response.type, response.method.c_str());
            return;
        }

        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "Resposta DISCOVERY recebida de %s", response.route.src.c_str());
        ESP_LOGI(TAG, "Node ID da topologia: %s", response.topology.node_id.c_str());
        ESP_LOGI(TAG, "Node ID do node_info: %s", response.topology.node_info.node_id.c_str());
        ESP_LOGI(TAG, "Node Type: %s", response.topology.node_info.node_type.c_str());
        ESP_LOGI(TAG, "Vizinhos: %u", (unsigned)response.topology.neighbors.size());

        NodeMappingInfo node_info;
        node_info.node_id = response.route.src;
        node_info.discovered_at_ms = esp_timer_get_time() / 1000ULL;

        if (!response.topology.node_id.empty())
        {
            node_info.node_id = response.topology.node_id;
        }
        if (!response.topology.node_name.empty())
        {
            node_info.node_name = response.topology.node_name;
        }
        node_info.has_gateway = response.topology.has_gateway;
        node_info.gateway_id = response.topology.gateway_id;
        node_info.neighbors = response.topology.neighbors;

        if (!response.topology.node_info.node_id.empty())
        {
            node_info.node_id = response.topology.node_info.node_id;
            node_info.node_name = response.topology.node_info.node_name;
            node_info.node_type = response.topology.node_info.node_type;
            node_info.capabilities = response.topology.node_info.capabilities;
            node_info.position_x = response.topology.node_info.position_x;
            node_info.position_y = response.topology.node_info.position_y;
        }

        if (!response.trace.hop_history.empty())
        {
            node_info.rssi = response.trace.hop_history.back().rssi;
        }

        if (node_info.node_type == "border")
        {
            ESP_LOGI(TAG, "Normalizando node_id do border node: %s -> border", node_info.node_id.c_str());
            node_info.node_id = "border";
        }

        if (s_collected_nodes.find(node_info.node_id) != s_collected_nodes.end())
        {
            ESP_LOGW(TAG, "Node %s já existe na coleta, atualizando informações...", node_info.node_id.c_str());
            NodeMappingInfo &existing = s_collected_nodes[node_info.node_id];
            if (!node_info.node_name.empty())
                existing.node_name = node_info.node_name;
            if (!node_info.node_type.empty())
                existing.node_type = node_info.node_type;
            existing.neighbors = node_info.neighbors;
            existing.has_gateway = node_info.has_gateway;
            existing.gateway_id = node_info.gateway_id;
            if (node_info.rssi != 0)
                existing.rssi = node_info.rssi;
            existing.discovered_at_ms = node_info.discovered_at_ms;
        }
        else
        {
            s_collected_nodes[node_info.node_id] = node_info;
        }
        ESP_LOGI(TAG, "Node mapeado: %s (nome: %s, tipo: %s, vizinhos: %u)",
                 node_info.node_id.c_str(),
                 node_info.node_name.empty() ? "(sem nome)" : node_info.node_name.c_str(),
                 node_info.node_type.empty() ? "normal" : node_info.node_type.c_str(),
                 (unsigned)node_info.neighbors.size());
        ESP_LOGI(TAG, "Total de nodes coletados até agora: %u", (unsigned)s_collected_nodes.size());
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    }

    bool NetworkMapper::is_mapping_in_progress()
    {
        return s_mapping_in_progress;
    }

    void NetworkMapper::send_discovery_packet()
    {
        Protocol::Packet discovery{};
        discovery.type = Protocol::PacketType::REQUEST;
        discovery.method = "DISCOVERY";
        discovery.route.src = "gateway";
        discovery.route.dst = "broadcast";
        discovery.endpoint = "/network/map";
        discovery.body = R"({"action":"discover"})";

        uint64_t now_ms = esp_timer_get_time() / 1000ULL;
        discovery.trace.packet_id = Protocol::generate_packet_id();
        discovery.trace.created_at_ms = now_ms;
        discovery.trace.path.push_back("gateway");
        discovery.routing_strategy = "flooding";
        discovery.ttl = 50;

        Protocol::HopInfo hop;
        hop.node_id = "gateway";
        hop.node_name = "Gateway Principal";
        hop.timestamp_ms = now_ms;
        hop.transport = "UART";
        discovery.trace.hop_history.push_back(hop);

        ESP_LOGI(TAG, "Enviando pacote DISCOVERY para border node...");
        Gateway::send_to_border(discovery);
    }

    void NetworkMapper::build_connectivity_matrix()
    {
        s_last_map.connectivity.connections.clear();
        s_last_map.mapped_at_ms = esp_timer_get_time() / 1000ULL;

        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "Construindo matriz de conectividade");
        ESP_LOGI(TAG, "Nodes coletados antes: %u", (unsigned)s_collected_nodes.size());

        std::string gateway_node_id = "gateway";
        if (s_collected_nodes.find(gateway_node_id) == s_collected_nodes.end())
        {
            ESP_LOGI(TAG, "Adicionando gateway ao mapa: %s", gateway_node_id.c_str());
            NodeMappingInfo gateway_node;
            gateway_node.node_id = gateway_node_id;
            gateway_node.node_name = "Gateway";
            gateway_node.node_type = "gateway";
            gateway_node.has_gateway = false;
            gateway_node.discovered_at_ms = esp_timer_get_time() / 1000ULL;
            gateway_node.rssi = 0;
            s_collected_nodes[gateway_node_id] = gateway_node;
            ESP_LOGI(TAG, "Gateway adicionado ao mapa");
        }

        if (LedManager::get_gateway_uart_connected())
        {
            std::string border_node_id = "border";
            if (s_collected_nodes.find(border_node_id) == s_collected_nodes.end())
            {
                ESP_LOGI(TAG, "Adicionando border node automaticamente: %s", border_node_id.c_str());
                NodeMappingInfo border_node;
                border_node.node_id = border_node_id;
                border_node.node_name = "Border Node";
                border_node.node_type = "border";
                border_node.has_gateway = true;
                border_node.gateway_id = "gateway";
                border_node.discovered_at_ms = esp_timer_get_time() / 1000ULL;
                border_node.rssi = 0;

                s_collected_nodes[border_node_id] = border_node;
                ESP_LOGI(TAG, "Border node adicionado ao mapa");
            }
            else
            {
                ESP_LOGI(TAG, "Border node já foi coletado via DISCOVERY_RESPONSE");
            }
        }

        ESP_LOGI(TAG, "Total de nodes após adicionar gateway e border: %u", (unsigned)s_collected_nodes.size());

        for (const auto &pair : s_collected_nodes)
        {
            const NodeMappingInfo &node = pair.second;
            ESP_LOGI(TAG, "  - Node: %s (tipo: %s, vizinhos: %u)",
                     node.node_id.c_str(),
                     node.node_type.empty() ? "normal" : node.node_type.c_str(),
                     (unsigned)node.neighbors.size());
        }

        for (const auto &pair : s_collected_nodes)
        {
            const NodeMappingInfo &node = pair.second;

            if (node.has_gateway && !node.gateway_id.empty())
            {
                Protocol::Connection conn{};
                conn.from_node_id = node.gateway_id;
                conn.to_node_id = node.node_id;
                conn.rssi = node.rssi;
                conn.is_direct = true;
                conn.last_communication_ms = node.discovered_at_ms;
                conn.packet_count = 1;

                ESP_LOGI(TAG, "Conexão gateway->border: %s -> %s, rssi=%d, packet_count=%u",
                         conn.from_node_id.c_str(), conn.to_node_id.c_str(),
                         conn.rssi, conn.packet_count);

                s_last_map.connectivity.connections.push_back(conn);
            }

            for (const auto &neighbor : node.neighbors)
            {
                Protocol::Connection conn{};
                conn.from_node_id = node.node_id;
                conn.to_node_id = neighbor.node_id;
                conn.rssi = neighbor.rssi;
                conn.is_direct = true;
                conn.last_communication_ms = neighbor.last_seen_ms;
                conn.packet_count = 1;

                ESP_LOGI(TAG, "Conexão node->neighbor: %s -> %s, rssi=%d, packet_count=%u",
                         conn.from_node_id.c_str(), conn.to_node_id.c_str(),
                         conn.rssi, conn.packet_count);

                s_last_map.connectivity.connections.push_back(conn);
            }
        }

        std::vector<Protocol::Connection> unique_connections;
        for (const auto &conn : s_last_map.connectivity.connections)
        {
            bool is_duplicate = false;
            for (const auto &existing : unique_connections)
            {
                if (existing.from_node_id == conn.from_node_id &&
                    existing.to_node_id == conn.to_node_id)
                {
                    is_duplicate = true;
                    break;
                }
            }
            if (!is_duplicate)
            {
                unique_connections.push_back(conn);
            }
        }
        s_last_map.connectivity.connections = unique_connections;

        size_t gateway_count = 0;
        size_t border_count = 0;
        size_t normal_node_count = 0;

        for (const auto &pair : s_collected_nodes)
        {
            if (pair.second.node_type == "gateway")
                gateway_count++;
            else if (pair.second.node_type == "border")
                border_count++;
            else
                normal_node_count++;
        }

        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "RESUMO DO MAPA:");
        ESP_LOGI(TAG, "  Gateways: %u", (unsigned)gateway_count);
        ESP_LOGI(TAG, "  Border Nodes: %u", (unsigned)border_count);
        ESP_LOGI(TAG, "  Nodes Normais: %u", (unsigned)normal_node_count);
        ESP_LOGI(TAG, "  TOTAL NODES: %u", (unsigned)s_collected_nodes.size());
        ESP_LOGI(TAG, "  TOTAL CONEXÕES: %u", (unsigned)s_last_map.connectivity.connections.size());
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    }

    void NetworkMapper::send_map_to_server(const NetworkMap &map)
    {
        ESP_LOGI(TAG, "Preparando envio: %u conexões no mapa", (unsigned)map.connectivity.connections.size());
        for (size_t i = 0; i < map.connectivity.connections.size(); i++)
        {
            const auto &conn = map.connectivity.connections[i];
            ESP_LOGI(TAG, "Conexão[%u]: %s -> %s, packet_count=%u, rssi=%d, last_comm=%llu",
                     (unsigned)i, conn.from_node_id.c_str(), conn.to_node_id.c_str(),
                     conn.packet_count, conn.rssi, conn.last_communication_ms);
        }

        std::string json = "{";
        json += "\"mapped_at_ms\":" + std::to_string(map.mapped_at_ms) + ",";
        json += "\"nodes\":[";

        bool first = true;
        ESP_LOGI(TAG, "Serializando %u nodes para JSON...", (unsigned)s_collected_nodes.size());
        for (const auto &pair : s_collected_nodes)
        {
            if (!first) json += ",";
            first = false;

            const NodeMappingInfo &node = pair.second;
            ESP_LOGI(TAG, "  Serializando node: %s (tipo: %s)",
                     node.node_id.c_str(),
                     node.node_type.empty() ? "normal" : node.node_type.c_str());

            json += "{";
            json += "\"node_id\":\"" + node.node_id + "\",";
            if (!node.node_name.empty())
                json += "\"node_name\":\"" + node.node_name + "\",";
            json += "\"node_type\":\"" + (node.node_type.empty() ? "normal" : node.node_type) + "\",";
            json += "\"rssi\":" + std::to_string(node.rssi) + ",";
            json += "\"discovered_at_ms\":" + std::to_string(node.discovered_at_ms) + ",";
            json += "\"position_x\":" + std::to_string(node.position_x) + ",";
            json += "\"position_y\":" + std::to_string(node.position_y) + ",";
            json += "\"neighbors_count\":" + std::to_string(node.neighbors.size());
            json += "}";
        }

        json += "],";
        json += "\"connectivity\":{";
        json += "\"connections\":[";

        first = true;
        for (const auto &conn : map.connectivity.connections)
        {
            if (!first) json += ",";
            first = false;

            ESP_LOGI(TAG, "Serializando conexão: %s -> %s, packet_count=%u",
                     conn.from_node_id.c_str(), conn.to_node_id.c_str(), conn.packet_count);

            json += "{";
            json += "\"from_node_id\":\"" + conn.from_node_id + "\",";
            json += "\"to_node_id\":\"" + conn.to_node_id + "\",";
            json += "\"rssi\":" + std::to_string(conn.rssi) + ",";
            json += "\"is_direct\":" + std::string(conn.is_direct ? "true" : "false") + ",";
            json += "\"last_communication_ms\":" + std::to_string(conn.last_communication_ms) + ",";
            json += "\"packet_count\":" + std::to_string(conn.packet_count);
            json += "}";
        }

        json += "]";
        json += "}";
        json += "}";

        ESP_LOGI(TAG, "JSON gerado (%u bytes)", (unsigned)json.size());

        Protocol::Packet map_packet{};
        map_packet.type = Protocol::PacketType::REQUEST;
        map_packet.method = "POST";
        map_packet.route.src = "gateway";
        map_packet.route.dst = "server";
        map_packet.endpoint = "/api/network/map";
        map_packet.body = json;

        ESP_LOGI(TAG, "Enviando mapeamento para servidor (%u bytes)...", (unsigned)json.size());
        Gateway::send_http_request(map_packet);
    }

    void NetworkMapper::mapping_task(void *param)
    {
        ESP_LOGI(TAG, "Task de mapeamento iniciada");

        for (;;)
        {
            if (s_periodic_interval_seconds > 0)
            {
                vTaskDelay(pdMS_TO_TICKS(s_periodic_interval_seconds * 1000));

                if (!s_mapping_in_progress)
                {
                    trigger_mapping();
                }
                else
                {
                    ESP_LOGD(TAG, "Mapeamento ainda em andamento, aguardando conclusão...");
                }
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            if (s_mapping_in_progress)
            {
                uint64_t now_ms = esp_timer_get_time() / 1000ULL;
                uint64_t elapsed = now_ms - s_mapping_start_time;

                if (elapsed >= s_mapping_timeout_ms)
                {
                    ESP_LOGI(TAG, "Timeout do mapeamento (%u ms), finalizando...", s_mapping_timeout_ms);

                    s_mapping_in_progress = false;

                    build_connectivity_matrix();

                    s_last_map.nodes.clear();
                    for (const auto &pair : s_collected_nodes)
                    {
                        s_last_map.nodes.push_back(pair.second);
                    }

                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    ESP_LOGI(TAG, "MAPEAMENTO CONCLUÍDO");
                    ESP_LOGI(TAG, "   Nodes mapeados: %u", (unsigned)s_last_map.nodes.size());
                    ESP_LOGI(TAG, "   Conexões: %u", (unsigned)s_last_map.connectivity.connections.size());
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

                    send_map_to_server(s_last_map);
                }
            }
        }
    }
}
