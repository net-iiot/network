#include "network_manager.hpp"
#include "esp_log.h"
#include "ble_transport.hpp"
#include "gateway.hpp"
#include "router.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_manager.hpp"
#include "espnow_transport.hpp"
#include "esp_timer.h"
#include "border_uart.hpp"
#include "chunk_manager.hpp"
#include "network_mapper.hpp"
#include "ota_manager.hpp"
#include "mbedtls/md5.h"
#include <cstring>
#include <sstream>

using namespace WetzelMesh;

static const char *TAG = "NETMAN";

bool NetworkManager::s_gateway = false;
std::vector<Neighbor> NetworkManager::s_neighbors;
std::string NetworkManager::s_network_id = "";
uint8_t NetworkManager::s_pmk[16] = {0};

uint64_t NetworkManager::now_ms() { return esp_timer_get_time() / 1000ULL; }

void NetworkManager::derive_pmk_from_network_id()
{
    if (s_network_id.empty())
    {
        ESP_LOGW(TAG, "Network ID vazio, usando PMK padrão");
        memset(s_pmk, 0, 16);
        return;
    }
    
    // Usa MD5 do Network ID para gerar PMK de 16 bytes
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, 
                      reinterpret_cast<const unsigned char*>(s_network_id.c_str()),
                      s_network_id.length());
    mbedtls_md5_finish(&ctx, s_pmk);
    mbedtls_md5_free(&ctx);
    
    ESP_LOGI(TAG, "PMK derivado do Network ID '%s'", s_network_id.c_str());
    ESP_LOGI(TAG, "PMK: %02X%02X%02X%02X...%02X%02X", 
             s_pmk[0], s_pmk[1], s_pmk[2], s_pmk[3],
             s_pmk[12], s_pmk[13], s_pmk[14], s_pmk[15]);
}

void NetworkManager::init(bool isGateway)
{
    s_gateway = isGateway;
    
    // Carrega Network ID do menuconfig
#ifdef CONFIG_WETZEL_NETWORK_ID
    s_network_id = CONFIG_WETZEL_NETWORK_ID;
#else
    s_network_id = "rede-01";  // Fallback
#endif
    
    // Deriva PMK do Network ID
    derive_pmk_from_network_id();
    
    // Obtém timeout configurado do menuconfig (padrão: 12000ms = 12s)
    uint32_t neighbor_timeout_ms = 12000; // Fallback se não configurado
#ifdef CONFIG_WETZEL_NEIGHBOR_TIMEOUT_MS
    neighbor_timeout_ms = CONFIG_WETZEL_NEIGHBOR_TIMEOUT_MS;
#endif
    
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "Network ID: %s", s_network_id.c_str());
    ESP_LOGI(TAG, "Canal WiFi: %u", get_wifi_channel());
    ESP_LOGI(TAG, "Timeout de Neighbors: %u ms (%.1f segundos)", 
             neighbor_timeout_ms, neighbor_timeout_ms / 1000.0f);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

    if (s_gateway)
    {
        // Gateway já foi inicializado no main.cpp com configurações WiFi
        ESP_LOGI(TAG, "INIT: GATEWAY (mesh/BLE OFF)");
    }
    else
    {
        ESPNOWTransport::init();
        BLETransport::init(false);

        BorderUart::init();
        LedManager::set_uart_enabled(BorderUart::is_enabled()); // NOVO: Atualiza estado do LED UART
        BorderUart::set_rx_handler([](const Protocol::Packet &pkt)
                                   {
            // Tudo que descer do gateway pela UART entra no roteador
            ESP_LOGI(TAG, "FLOW: UART(BORDER<-GW) -> Router");
            Router::handle_packet(pkt); });

        xTaskCreatePinnedToCore(&NetworkManager::refresh_neighbors_task, "neighbors", 4096, nullptr, 4, nullptr, 0);
        start_hello_task();

        // Registra callback de report OTA: node envia pacote OTA_RESULT pela mesh para o gateway
        OTAManager::set_report_callback([](bool success, const std::string &command_id, const std::string &node_id, const std::string &error_msg) {
            std::string my_id = node_id.empty() ? BLETransport::node_id() : node_id;

            Protocol::Packet result;
            result.type = Protocol::PacketType::EVENT;
            result.method = "OTA_RESULT";
            result.route.src = my_id;
            result.route.dst = "gateway";

            std::ostringstream body;
            body << R"({"command_id":")" << command_id
                 << R"(","node_id":")" << my_id
                 << R"(","success":)" << (success ? "true" : "false");
            if (!error_msg.empty())
                body << R"(,"error":")" << error_msg << "\"";
            body << "}";
            result.body = body.str();

            ESP_LOGI("NETMAN", "Enviando OTA_RESULT: cmd=%s, node=%s, success=%s",
                     command_id.c_str(), my_id.c_str(), success ? "true" : "false");
            Router::handle_packet(result);
        });

        ESP_LOGI(TAG, "INIT: NODE (mesh+BLE [+UART se borda])");
    }
}

std::string NetworkManager::get_network_id()
{
    return s_network_id;
}

const uint8_t* NetworkManager::get_pmk()
{
    return s_pmk;
}

uint8_t NetworkManager::get_wifi_channel()
{
#ifdef CONFIG_WETZEL_WIFI_CHANNEL
    return CONFIG_WETZEL_WIFI_CHANNEL;
#else
    // Fallback: deriva do Network ID se não configurado
    if (s_network_id.empty())
        return 1;
    
    uint32_t hash = 0;
    for (char c : s_network_id)
    {
        hash = hash * 31 + c;
    }
    
    // Canais recomendados (1, 6, 11 não se sobrepõem)
    uint8_t channels[] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 5, 10};
    return channels[hash % (sizeof(channels) / sizeof(channels[0]))];
#endif
}

void NetworkManager::refresh_neighbors_task(void *param)
{
    // Obtém timeout configurado do menuconfig (padrão: 12000ms = 12s)
    // Lê apenas uma vez, pois é uma constante de compilação
    uint32_t neighbor_timeout_ms = 12000; // Fallback se não configurado
#ifdef CONFIG_WETZEL_NEIGHBOR_TIMEOUT_MS
    neighbor_timeout_ms = CONFIG_WETZEL_NEIGHBOR_TIMEOUT_MS;
#endif
    
    ESP_LOGI(TAG, "Task de refresh de neighbors iniciada (timeout: %u ms)", neighbor_timeout_ms);
    
    for (;;)
    {
        const uint64_t now = now_ms();
        bool any_recent = false;

        for (auto it = s_neighbors.begin(); it != s_neighbors.end();)
        {
            if ((now - it->last_seen_ms) > neighbor_timeout_ms)
            {
                // ✅ NOVO: Salva ID do node removido antes de apagar
                std::string removed_node_id = it->id;
                
                // ✅ NOVO: Notifica gateway se for border node
                if (BorderUart::is_enabled())
                {
                    Protocol::Packet notification;
                    notification.type = Protocol::PacketType::EVENT;
                    notification.method = "NODE_LEFT";
                    notification.route.src = BLETransport::node_id();
                    notification.route.dst = "gateway";
                    
                    // Cria JSON com informações do node removido
                    std::ostringstream body;
                    body << R"({"node_id":")" << removed_node_id << "}";
                    notification.body = body.str();
                    
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    ESP_LOGI(TAG, "NODE REMOVIDO (timeout): %s", removed_node_id.c_str());
                    ESP_LOGI(TAG, "Notificando gateway...");
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    BorderUart::send_to_gateway(notification);
                }
                
                it = s_neighbors.erase(it);
            }
            else
            {
                any_recent = true;
                ++it;
            }
        }

        if (!s_gateway)
            LedManager::set_node_joined(any_recent);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

bool NetworkManager::is_gateway() { return s_gateway; }
const std::vector<Neighbor> &NetworkManager::neighbors() { return s_neighbors; }

bool NetworkManager::send(const Protocol::Packet &p)
{
    if (s_gateway)
    {
        ESP_LOGI(TAG, "ROUTE DECISION: GATEWAY TX via UART -> BORDER (dst=%s)", p.route.dst.c_str());
        return Gateway::send_to_border(p);
    }
    else
    {
        if (p.route.dst == "gateway" && BorderUart::is_enabled())
        {
            ESP_LOGI(TAG, "ROUTE DECISION: NODE (BORDER) TX via UART -> GW (dst=%s)", p.route.dst.c_str());
            return BorderUart::send_to_gateway(p);
        }
        ESP_LOGI(TAG, "ROUTE DECISION: NODE TX via MESH (dst=%s)", p.route.dst.c_str());
        return ESPNOWTransport::send(p);
    }
}

void NetworkManager::handle_incoming(const Protocol::Packet &packet)
{
    ESP_LOGI(TAG, "HANDLE_INCOMING: %s -> %s", packet.route.src.c_str(), packet.route.dst.c_str());

    if (s_gateway)
    {
        if (packet.route.dst == "server")
        {
            ESP_LOGI(TAG, "FORWARD: GW -> SERVER");
            Gateway::send(packet);
        }
        else
        {
            ESP_LOGI(TAG, "FORWARD: GW -> BORDER via UART");
            Gateway::send_to_border(packet);
        }
        return;
    }

    // Node
    if (packet.type == Protocol::PacketType::EVENT && packet.method == "HELLO")
    {
        // Processa topologia recebida (para visualização futura)
        if (!packet.topology.node_id.empty())
        {
            ESP_LOGI(TAG, "Topologia recebida de %s: %u vizinhos", 
                     packet.topology.node_id.c_str(), 
                     (unsigned)packet.topology.neighbors.size());
        }
        return;
    }
    
    // Processa DISCOVERY - nodes devem responder com DISCOVERY_RESPONSE
    if (packet.type == Protocol::PacketType::REQUEST && packet.method == "DISCOVERY")
    {
        ESP_LOGI(TAG, "Pacote DISCOVERY recebido de %s", packet.route.src.c_str());
        
        // Cria resposta DISCOVERY_RESPONSE
        Protocol::Packet response;
        response.type = Protocol::PacketType::RESPONSE;
        response.method = "DISCOVERY_RESPONSE";
        response.route.src = BLETransport::node_id();
        response.route.dst = packet.route.src;
        response.status = 200;
        response.request_id = packet.trace.packet_id;
        
        // Preenche informações de topologia
        response.topology.node_id = BLETransport::node_id();
        response.topology.network_id = s_network_id;
        response.topology.has_gateway = BorderUart::is_enabled();
        response.topology.gateway_id = BorderUart::is_enabled() ? "gateway" : "";
        
        // ✅ ADICIONAR: Preenche node_type na resposta
        response.topology.node_info.node_id = BLETransport::node_id();
        response.topology.node_info.node_type = BorderUart::is_enabled() ? "border" : "normal";
        
        // Adiciona vizinhos
        for (const auto &nbr : s_neighbors)
        {
            Protocol::NeighborInfo nbr_info;
            nbr_info.node_id = nbr.id;
            nbr_info.rssi = nbr.rssi;
            nbr_info.last_seen_ms = nbr.last_seen_ms;
            response.topology.neighbors.push_back(nbr_info);
        }
        
        // Adiciona trace
        response.trace = packet.trace;
        response.trace.path.push_back(BLETransport::node_id());
        response.trace.hop_count++;
        
        Protocol::HopInfo hop;
        hop.node_id = BLETransport::node_id();
        hop.timestamp_ms = now_ms();
        hop.transport = BorderUart::is_enabled() ? "UART" : "MESH";
        response.trace.hop_history.push_back(hop);
        
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "Enviando DISCOVERY_RESPONSE com %u vizinhos", (unsigned)response.topology.neighbors.size());
        ESP_LOGI(TAG, "Node ID: %s, Destino: %s", response.route.src.c_str(), response.route.dst.c_str());
        ESP_LOGI(TAG, "Border UART habilitado: %s", BorderUart::is_enabled() ? "SIM" : "NÃO");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        // Envia resposta de volta para o gateway
        if (BorderUart::is_enabled())
        {
            ESP_LOGI(TAG, "Enviando DISCOVERY_RESPONSE via UART (border node)");
            BorderUart::send_to_gateway(response);
        }
        else
        {
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
            ESP_LOGI(TAG, "NODE COMUM: Enviando DISCOVERY_RESPONSE via MESH");
            ESP_LOGI(TAG, "   Node ID: %s", response.route.src.c_str());
            ESP_LOGI(TAG, "   Destino: %s", response.route.dst.c_str());
            ESP_LOGI(TAG, "   Node Type: %s", response.topology.node_info.node_type.c_str());
            ESP_LOGI(TAG, "   Vizinhos: %u", (unsigned)response.topology.neighbors.size());
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
            // Envia diretamente pela mesh para o gateway (ou para o border node que vai reencaminhar)
            // O destino é o gateway, então envia via mesh
            bool sent = ESPNOWTransport::send(response);
            if (sent)
            {
                ESP_LOGI(TAG, "✅ DISCOVERY_RESPONSE enviada com sucesso via MESH");
            }
            else
            {
                ESP_LOGE(TAG, "❌ FALHA ao enviar DISCOVERY_RESPONSE via MESH");
            }
        }
        
        // IMPORTANTE: Se o DISCOVERY é broadcast, também precisa reencaminhar para outros nodes
        // (flooding) para que todos os nodes recebam e respondam
        if (packet.route.dst == "broadcast")
        {
            ESP_LOGI(TAG, "Reencaminhando DISCOVERY (broadcast) para outros nodes da mesh...");
            Router::handle_packet(packet);
        }
        
        return;
    }
    
    // Processa comandos OTA_START
    if (packet.type == Protocol::PacketType::REQUEST && packet.method == "OTA_START")
    {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "COMANDO OTA_START RECEBIDO");
        ESP_LOGI(TAG, "   De: %s", packet.route.src.c_str());
        ESP_LOGI(TAG, "   Para: %s", packet.route.dst.c_str());
        ESP_LOGI(TAG, "   Body: %s", packet.body.c_str());
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        // Verifica se o comando é para este node ou para broadcast/all_nodes
        std::string my_id = BLETransport::node_id();
        bool is_for_me = (packet.route.dst == my_id || 
                         packet.route.dst == "broadcast" || 
                         packet.route.dst == "all_nodes");
        
        if (is_for_me)
        {
            // Processa comando OTA
            std::string json = Protocol::serialize(packet);
            OTAManager::handle_ota_packet(json);
        }
        else
        {
            // Comando não é para este node, apenas reencaminha
            ESP_LOGI(TAG, "Comando OTA não é para este node (%s), reencaminhando...", my_id.c_str());
        }
        
        // Reencaminha comando OTA para outros nodes (se broadcast ou all_nodes)
        if (packet.route.dst == "broadcast" || packet.route.dst == "all_nodes")
        {
            Router::handle_packet(packet);
        }
        
        return;
    }
    
    // Processa chunks - se for chunk, tenta reconstruir
    if (packet.is_chunk)
    {
        ESP_LOGI(TAG, "Chunk recebido: %u/%u (id=%u)", 
                 packet.chunk_index + 1, packet.chunk_total, packet.chunk_id);
        
        Protocol::Packet reconstructed;
        if (ChunkManager::instance().add_chunk(packet, reconstructed))
        {
            // Mensagem completa reconstruída, processa normalmente
            ESP_LOGI(TAG, "Mensagem reconstruída de chunks, processando...");
            handle_incoming(reconstructed); // Processa a mensagem completa
            return;
        }
        // Ainda faltam chunks, apenas aguarda
        return;
    }

    // Processa DISCOVERY_RESPONSE de nodes comuns que chegam via mesh no border node
    // Se o border node recebe DISCOVERY_RESPONSE via mesh, reencaminha para gateway via UART
    // IMPORTANTE: Isso deve ser ANTES do check genérico de "gateway" abaixo
    if (packet.type == Protocol::PacketType::RESPONSE && 
        packet.method == "DISCOVERY_RESPONSE" && 
        packet.route.dst == "gateway" &&
        BorderUart::is_enabled())
    {
        // Verifica se o pacote NÃO veio do próprio border node (evita loop)
        std::string my_id = BLETransport::node_id();
        if (packet.route.src != my_id && packet.route.src != "border")
        {
            ESP_LOGI(TAG, "FORWARD: DISCOVERY_RESPONSE de %s (via MESH) -> GW via UART", packet.route.src.c_str());
            BorderUart::send_to_gateway(packet);
            return;
        }
    }
    
    // Pacotes com destino "gateway" - roteia via UART (se border) ou MESH (se node comum)
    if (packet.route.dst == "gateway")
    {
        if (BorderUart::is_enabled())
        {
            ESP_LOGI(TAG, "FORWARD: NODE(BORDER) -> GW via UART");
            BorderUart::send_to_gateway(packet);
        }
        else
        {
            ESP_LOGI(TAG, "FORWARD: NODE -> GW via MESH");
            ESPNOWTransport::send(packet);
        }
        return;
    }

    // Broadcast/controlado — habilite se quiser propagar
    // if (packet.route.dst == "broadcast") ESPNOWTransport::send(packet);
}

void NetworkManager::start_hello_task()
{
    auto hello_task = [](void *)
    {
        vTaskDelay(pdMS_TO_TICKS(400));
        for (;;)
        {
            Protocol::Packet hello{};
            hello.type = Protocol::PacketType::EVENT;
            hello.method = "HELLO";
            hello.route.src = BLETransport::node_id();
            hello.route.dst = "broadcast";
            hello.body = R"({"t":"hello"})";
            
            // Adiciona informações de topologia
            hello.topology.node_id = BLETransport::node_id();
            hello.topology.has_gateway = BorderUart::is_enabled();
            if (hello.topology.has_gateway)
            {
                hello.topology.gateway_id = "gateway";
            }
            
            // Adiciona lista de vizinhos
            const auto &neighbors = NetworkManager::neighbors();
            for (const auto &nbr : neighbors)
            {
                Protocol::NeighborInfo nbr_info;
                nbr_info.node_id = nbr.id;
                nbr_info.rssi = nbr.rssi;
                nbr_info.last_seen_ms = nbr.last_seen_ms;
                hello.topology.neighbors.push_back(nbr_info);
            }

            ESP_LOGI(TAG, "TX[HELLO] %s -> broadcast (vizinhos=%u)", 
                     hello.route.src.c_str(), (unsigned)hello.topology.neighbors.size());
            // blink no hello será tratado dentro do ESPNOWTransport::send(hello);
            ESPNOWTransport::send(hello);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    };
    xTaskCreatePinnedToCore(hello_task, "hello_task", 4096, nullptr, 4, nullptr, tskNO_AFFINITY);
}

void NetworkManager::on_hello(const Protocol::Packet &hello_packet, int rssi)
{
    // Verifica Network ID - ignora nodes de outras redes
    std::string received_network_id = hello_packet.topology.network_id;
    if (!received_network_id.empty() && received_network_id != s_network_id)
    {
        ESP_LOGD(TAG, "HELLO ignorado: Network ID diferente (recebido: %s, esperado: %s)",
                 received_network_id.c_str(), s_network_id.c_str());
        return;  // Ignora nodes de outras redes
    }
    
    const std::string &node_id = hello_packet.route.src;
    const uint64_t now = now_ms();
    bool is_new_node = true;
    
    for (auto &n : s_neighbors)
    {
        if (n.id == node_id)
        {
            n.last_seen_ms = now;
            n.rssi = rssi;
            is_new_node = false;
            return;
        }
    }
    
    // Novo node detectado (da mesma rede)
    if (is_new_node)
    {
        s_neighbors.push_back({node_id, rssi, now});
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "NOVO NODE DETECTADO (Rede: %s): %s (RSSI: %d)", 
                 s_network_id.c_str(), node_id.c_str(), rssi);
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        // ✅ NOVO: Se for border node, notifica gateway imediatamente
        if (BorderUart::is_enabled())
        {
            Protocol::Packet notification;
            notification.type = Protocol::PacketType::EVENT;
            notification.method = "NODE_JOINED";
            notification.route.src = BLETransport::node_id();
            notification.route.dst = "gateway";
            
            // Cria JSON com informações do novo node
            std::ostringstream body;
            body << R"({"node_id":")" << node_id << R"(","rssi":)" << rssi << "}";
            notification.body = body.str();
            
            ESP_LOGI(TAG, "Notificando gateway sobre novo node: %s", node_id.c_str());
            BorderUart::send_to_gateway(notification);
        }
        
        // Dispara mapeamento se for gateway (não deveria acontecer, mas mantém por segurança)
        if (s_gateway)
        {
            ESP_LOGI(TAG, "Disparando mapeamento devido a novo node detectado...");
            NetworkMapper::trigger_mapping();
        }
    }
}
