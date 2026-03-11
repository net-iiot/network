#include "network_manager.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "ble_transport.hpp"
#include "gateway.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_manager.hpp"
#include "espnow_transport.hpp"
#include "esp_timer.h"
#include "border_uart.hpp"
#include "protocol.hpp"
#include "network_mapper.hpp"
#include "ota_manager.hpp"
#include "mbedtls/md5.h"
#include <cstring>
#include <sstream>
#include <mutex>

using namespace NetworkMesh;

static const char *TAG = "NETMAN";

bool NetworkManager::s_gateway = false;
std::vector<Neighbor> NetworkManager::s_neighbors;
std::mutex NetworkManager::s_neighbors_mutex;
std::string NetworkManager::s_network_id = "";
uint8_t NetworkManager::s_pmk[16] = {0};
PacketHandler NetworkManager::s_packet_handler;

uint64_t NetworkManager::now_ms() { return esp_timer_get_time() / 1000ULL; }

PacketHandler &NetworkManager::packet_handler() { return s_packet_handler; }

void NetworkManager::derive_pmk_from_network_id()
{
    if (s_network_id.empty())
    {
        ESP_LOGW(TAG, "Network ID vazio, usando PMK padrao");
        memset(s_pmk, 0, 16);
        return;
    }

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx,
                       reinterpret_cast<const unsigned char *>(s_network_id.c_str()),
                       s_network_id.length());
    mbedtls_md5_finish(&ctx, s_pmk);
    mbedtls_md5_free(&ctx);

    ESP_LOGI(TAG, "PMK derivado do Network ID '%s'", s_network_id.c_str());
    ESP_LOGI(TAG, "PMK: %02X%02X%02X%02X...%02X%02X",
             s_pmk[0], s_pmk[1], s_pmk[2], s_pmk[3],
             s_pmk[12], s_pmk[13], s_pmk[14], s_pmk[15]);
}

void NetworkManager::route_packet(const Protocol::Packet &pkt)
{
    ESP_LOGI(TAG, "Roteando pacote: %s -> %s",
             pkt.route.src.c_str(), pkt.route.dst.c_str());

    if (pkt.type == Protocol::PacketType::EVENT && pkt.method == std::string("HELLO"))
        return;

    if (s_gateway)
    {
        if (pkt.route.dst == "server")
            Gateway::send(pkt);
        else
            Gateway::send_to_border(pkt);
        return;
    }

    if (pkt.route.dst == "gateway" || pkt.route.dst == "server")
    {
        if (BorderUart::is_enabled())
        {
            ESP_LOGI(TAG, "Pacote para %s, enviando via UART (border node)", pkt.route.dst.c_str());
            BorderUart::send_to_gateway(pkt);
            return;
        }
        ESP_LOGI(TAG, "Pacote para %s, reencaminhando pela mesh (node comum)", pkt.route.dst.c_str());
    }

    if (pkt.route.dst == "gateway" || pkt.route.dst == "server" || pkt.route.dst == "broadcast" ||
        (pkt.type == Protocol::PacketType::EVENT && pkt.method == "DATA") ||
        (pkt.type == Protocol::PacketType::REQUEST && pkt.method == "DISCOVERY"))
    {
        Protocol::Packet updated_pkt = pkt;
        std::string current_node_id = BLETransport::node_id();
        uint64_t now = esp_timer_get_time() / 1000ULL;

        for (const auto &node : updated_pkt.trace.path)
        {
            if (node == current_node_id)
            {
                ESP_LOGW(TAG, "Loop detectado: node %s ja esta no path, descartando", current_node_id.c_str());
                return;
            }
        }

        if (updated_pkt.ttl == 0)
        {
            ESP_LOGW(TAG, "TTL expirado, descartando pacote");
            return;
        }

        updated_pkt.ttl--;
        updated_pkt.trace.path.push_back(current_node_id);
        updated_pkt.trace.hop_count++;
        updated_pkt.trace.received_at_ms = now;

        Protocol::HopInfo hop;
        hop.node_id = current_node_id;
        hop.timestamp_ms = now;
        hop.transport = "MESH";
        updated_pkt.trace.hop_history.push_back(hop);

        ESP_LOGI(TAG, "Node %s recebeu dado da mesh (hop %u) - aguardando 1s antes de repassar",
                 current_node_id.c_str(), updated_pkt.trace.hop_count);
        LedManager::set_led_on_for_duration(1000);
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_LOGI(TAG, "Repassando dado apos backoff...");
        ESPNOWTransport::send(updated_pkt);
        return;
    }
}

void NetworkManager::init(bool isGateway)
{
    s_gateway = isGateway;

    s_packet_handler = [](const Protocol::Packet &pkt)
    { route_packet(pkt); };

#ifdef CONFIG_NETWORK_NETWORK_ID
    s_network_id = CONFIG_NETWORK_NETWORK_ID;
#else
    s_network_id = "rede-01";
#endif

    derive_pmk_from_network_id();

    uint32_t neighbor_timeout_ms = 12000;
#ifdef CONFIG_NETWORK_NEIGHBOR_TIMEOUT_MS
    neighbor_timeout_ms = CONFIG_NETWORK_NEIGHBOR_TIMEOUT_MS;
#endif

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "Network ID: %s", s_network_id.c_str());
    ESP_LOGI(TAG, "Canal WiFi: %u", get_wifi_channel());
    ESP_LOGI(TAG, "Timeout de Neighbors: %u ms (%.1f segundos)",
             neighbor_timeout_ms, neighbor_timeout_ms / 1000.0f);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

    if (s_gateway)
    {
        ESP_LOGI(TAG, "INIT: GATEWAY (mesh/BLE OFF)");
    }
    else
    {
        ESPNOWTransport::init();
        BLETransport::init(false);

        BorderUart::init();
        LedManager::set_uart_enabled(BorderUart::is_enabled());
        BorderUart::set_rx_handler([](const Protocol::Packet &pkt)
                                   {
            ESP_LOGI(TAG, "FLOW: UART(BORDER<-GW) -> handle_incoming");
            handle_incoming(pkt); });

        xTaskCreatePinnedToCore(&NetworkManager::refresh_neighbors_task, "neighbors", 4096, nullptr, 4, nullptr, 0);
        start_hello_task();

        OTAManager::set_report_callback([](bool success, const std::string &command_id, const std::string &node_id, const std::string &error_msg)
                                        {
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
            route_packet(result); });

        ESP_LOGI(TAG, "INIT: NODE (mesh+BLE [+UART se borda])");
    }
}

std::string NetworkManager::get_network_id()
{
    return s_network_id;
}

const uint8_t *NetworkManager::get_pmk()
{
    return s_pmk;
}

uint8_t NetworkManager::get_wifi_channel()
{
#ifdef CONFIG_NETWORK_WIFI_CHANNEL
    return CONFIG_NETWORK_WIFI_CHANNEL;
#else
    if (s_network_id.empty())
        return 1;

    uint32_t hash = 0;
    for (char c : s_network_id)
        hash = hash * 31 + c;

    uint8_t channels[] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 5, 10};
    return channels[hash % (sizeof(channels) / sizeof(channels[0]))];
#endif
}

void NetworkManager::refresh_neighbors_task(void *param)
{
    uint32_t neighbor_timeout_ms = 12000;
#ifdef CONFIG_NETWORK_NEIGHBOR_TIMEOUT_MS
    neighbor_timeout_ms = CONFIG_NETWORK_NEIGHBOR_TIMEOUT_MS;
#endif

    ESP_LOGI(TAG, "Task de refresh de neighbors iniciada (timeout: %u ms)", neighbor_timeout_ms);

    for (;;)
    {
        const uint64_t now = now_ms();
        bool any_recent = false;
        std::vector<std::string> removed_ids;

        {
            std::lock_guard<std::mutex> lock(s_neighbors_mutex);
            for (auto it = s_neighbors.begin(); it != s_neighbors.end();)
            {
                if ((now - it->last_seen_ms) > neighbor_timeout_ms)
                {
                    removed_ids.push_back(it->id);
                    it = s_neighbors.erase(it);
                }
                else
                {
                    any_recent = true;
                    ++it;
                }
            }
        }

        if (BorderUart::is_enabled())
        {
            for (const auto &removed_node_id : removed_ids)
            {
                Protocol::Packet notification;
                notification.type = Protocol::PacketType::EVENT;
                notification.method = "NODE_LEFT";
                notification.route.src = BLETransport::node_id();
                notification.route.dst = "gateway";

                std::ostringstream body;
                body << R"({"node_id":")" << removed_node_id << "}";
                notification.body = body.str();

                ESP_LOGI(TAG, "NODE REMOVIDO (timeout): %s - Notificando gateway...", removed_node_id.c_str());
                BorderUart::send_to_gateway(notification);
            }
        }

        if (!s_gateway)
            LedManager::set_node_joined(any_recent);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

bool NetworkManager::is_gateway() { return s_gateway; }
std::vector<Neighbor> NetworkManager::neighbors()
{
    std::lock_guard<std::mutex> lock(s_neighbors_mutex);
    return s_neighbors;
}

bool NetworkManager::send(const Protocol::Packet &p)
{
    if (s_gateway)
    {
        ESP_LOGI(TAG, "ROUTE: GATEWAY TX via UART -> BORDER (dst=%s)", p.route.dst.c_str());
        return Gateway::send_to_border(p);
    }

    if (p.route.dst == "gateway" && BorderUart::is_enabled())
    {
        ESP_LOGI(TAG, "ROUTE: NODE (BORDER) TX via UART -> GW (dst=%s)", p.route.dst.c_str());
        return BorderUart::send_to_gateway(p);
    }

    ESP_LOGI(TAG, "ROUTE: NODE TX via MESH (dst=%s)", p.route.dst.c_str());
    return ESPNOWTransport::send(p);
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

    if (packet.type == Protocol::PacketType::EVENT && packet.method == "HELLO")
    {
        if (!packet.topology.node_id.empty())
        {
            ESP_LOGI(TAG, "Topologia recebida de %s: %u vizinhos",
                     packet.topology.node_id.c_str(),
                     (unsigned)packet.topology.neighbors.size());
        }
        return;
    }

    if (packet.type == Protocol::PacketType::REQUEST && packet.method == "DISCOVERY")
    {
        ESP_LOGI(TAG, "Pacote DISCOVERY recebido de %s", packet.route.src.c_str());

        Protocol::Packet response;
        response.type = Protocol::PacketType::RESPONSE;
        response.method = "DISCOVERY_RESPONSE";
        response.route.src = BLETransport::node_id();
        response.route.dst = packet.route.src;
        response.status = 200;
        response.request_id = packet.trace.packet_id;

        response.topology.node_id = BLETransport::node_id();
        response.topology.network_id = s_network_id;
        response.topology.has_gateway = BorderUart::is_enabled();
        response.topology.gateway_id = BorderUart::is_enabled() ? "gateway" : "";

        response.topology.node_info.node_id = BLETransport::node_id();
        response.topology.node_info.node_type = BorderUart::is_enabled() ? "border" : "normal";

        auto current_neighbors = neighbors();
        for (const auto &nbr : current_neighbors)
        {
            Protocol::NeighborInfo nbr_info;
            nbr_info.node_id = nbr.id;
            nbr_info.rssi = nbr.rssi;
            nbr_info.last_seen_ms = nbr.last_seen_ms;
            response.topology.neighbors.push_back(nbr_info);
        }

        response.trace = packet.trace;
        response.trace.path.push_back(BLETransport::node_id());
        response.trace.hop_count++;

        Protocol::HopInfo hop;
        hop.node_id = BLETransport::node_id();
        hop.timestamp_ms = now_ms();
        hop.transport = BorderUart::is_enabled() ? "UART" : "MESH";
        response.trace.hop_history.push_back(hop);

        ESP_LOGI(TAG, "Enviando DISCOVERY_RESPONSE com %u vizinhos", (unsigned)response.topology.neighbors.size());

        if (BorderUart::is_enabled())
        {
            ESP_LOGI(TAG, "Enviando DISCOVERY_RESPONSE via UART (border node)");
            BorderUart::send_to_gateway(response);
        }
        else
        {
            ESP_LOGI(TAG, "Enviando DISCOVERY_RESPONSE via MESH");
            ESPNOWTransport::send(response);
        }

        if (packet.route.dst == "broadcast")
        {
            ESP_LOGI(TAG, "Reencaminhando DISCOVERY (broadcast) para mesh...");
            route_packet(packet);
        }

        return;
    }

    if (packet.type == Protocol::PacketType::REQUEST && packet.method == "OTA_START")
    {
        ESP_LOGI(TAG, "COMANDO OTA_START RECEBIDO de: %s para: %s", packet.route.src.c_str(), packet.route.dst.c_str());

        std::string my_id = BLETransport::node_id();
        bool is_for_me = (packet.route.dst == my_id ||
                          packet.route.dst == "broadcast" ||
                          packet.route.dst == "all_nodes" ||
                          (packet.route.dst == "border" && BorderUart::is_enabled()));

        if (is_for_me)
        {
            std::string json = Protocol::serialize(packet);
            OTAManager::handle_ota_packet(json);
        }

        if (packet.route.dst == "broadcast" || packet.route.dst == "all_nodes")
            route_packet(packet);

        return;
    }

    if (packet.is_chunk)
    {
        ESP_LOGI(TAG, "Chunk recebido: %u/%u (id=%u)",
                 packet.chunk_index + 1, packet.chunk_total, packet.chunk_id);

        Protocol::Packet reconstructed;
        if (Protocol::ChunkManager::instance().add_chunk(packet, reconstructed))
        {
            ESP_LOGI(TAG, "Mensagem reconstruida de chunks, processando...");
            handle_incoming(reconstructed);
            return;
        }
        return;
    }

    if (packet.type == Protocol::PacketType::RESPONSE &&
        packet.method == "DISCOVERY_RESPONSE" &&
        packet.route.dst == "gateway" &&
        BorderUart::is_enabled())
    {
        std::string my_id = BLETransport::node_id();
        if (packet.route.src != my_id && packet.route.src != "border")
        {
            ESP_LOGI(TAG, "FORWARD: DISCOVERY_RESPONSE de %s (via MESH) -> GW via UART", packet.route.src.c_str());
            BorderUart::send_to_gateway(packet);
            return;
        }
    }

    if (packet.route.dst == "gateway")
    {
        if (packet.route.src == "gateway")
        {
            ESP_LOGW(TAG, "Pacote com src=gateway e dst=gateway descartado (loop)");
            return;
        }
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

            hello.topology.node_id = BLETransport::node_id();
            hello.topology.network_id = NetworkManager::get_network_id();
            hello.topology.has_gateway = BorderUart::is_enabled();
            if (hello.topology.has_gateway)
                hello.topology.gateway_id = "gateway";

            auto nbrs = NetworkManager::neighbors();
            for (const auto &nbr : nbrs)
            {
                Protocol::NeighborInfo nbr_info;
                nbr_info.node_id = nbr.id;
                nbr_info.rssi = nbr.rssi;
                nbr_info.last_seen_ms = nbr.last_seen_ms;
                hello.topology.neighbors.push_back(nbr_info);
            }

            ESP_LOGI(TAG, "TX[HELLO] %s -> broadcast (vizinhos=%u)",
                     hello.route.src.c_str(), (unsigned)hello.topology.neighbors.size());
            ESPNOWTransport::send(hello);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    };
    xTaskCreatePinnedToCore(hello_task, "hello_task", 6144, nullptr, 4, nullptr, tskNO_AFFINITY);
}

void NetworkManager::on_hello(const Protocol::Packet &hello_packet, int rssi)
{
    std::string received_network_id = hello_packet.topology.network_id;
    if (!received_network_id.empty() && received_network_id != s_network_id)
    {
        ESP_LOGD(TAG, "HELLO ignorado: Network ID diferente (recebido: %s, esperado: %s)",
                 received_network_id.c_str(), s_network_id.c_str());
        return;
    }

    const std::string &node_id = hello_packet.route.src;
    const uint64_t now = now_ms();
    bool is_new_node = true;

    {
        std::lock_guard<std::mutex> lock(s_neighbors_mutex);
        for (auto &n : s_neighbors)
        {
            if (n.id == node_id)
            {
                n.last_seen_ms = now;
                n.rssi = rssi;
                is_new_node = false;
                break;
            }
        }
        if (is_new_node)
            s_neighbors.push_back({node_id, rssi, now});
    }

    if (is_new_node)
    {
        ESP_LOGI(TAG, "NOVO NODE DETECTADO (Rede: %s): %s (RSSI: %d)",
                 s_network_id.c_str(), node_id.c_str(), rssi);

        if (BorderUart::is_enabled())
        {
            Protocol::Packet notification;
            notification.type = Protocol::PacketType::EVENT;
            notification.method = "NODE_JOINED";
            notification.route.src = BLETransport::node_id();
            notification.route.dst = "gateway";

            std::ostringstream body;
            body << R"({"node_id":")" << node_id << R"(","rssi":)" << rssi << "}";
            notification.body = body.str();

            ESP_LOGI(TAG, "Notificando gateway sobre novo node: %s", node_id.c_str());
            BorderUart::send_to_gateway(notification);
        }

        if (s_gateway)
        {
            ESP_LOGI(TAG, "Disparando mapeamento devido a novo node detectado...");
            NetworkMapper::trigger_mapping();
        }
    }
}
