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
#include "test_packet_generator.hpp"

using namespace WetzelMesh;

static const char *TAG = "NETMAN";

bool NetworkManager::s_gateway = false;
std::vector<Neighbor> NetworkManager::s_neighbors;

uint64_t NetworkManager::now_ms() { return esp_timer_get_time() / 1000ULL; }

void NetworkManager::init(bool isGateway)
{
    s_gateway = isGateway;

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
        ESP_LOGI(TAG, "INIT: NODE (mesh+BLE [+UART se borda])");
    }
}

void NetworkManager::refresh_neighbors_task(void *param)
{
    for (;;)
    {
        const uint64_t now = now_ms();
        bool any_recent = false;

        for (auto it = s_neighbors.begin(); it != s_neighbors.end();)
        {
            if ((now - it->last_seen_ms) > 8000)
                it = s_neighbors.erase(it);
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
    
    // Processa TOKEN (modo teste)
#ifdef CONFIG_WETZEL_TEST_MODE
    if (packet.type == Protocol::PacketType::EVENT && packet.method == "TOKEN")
    {
        std::string my_id = BLETransport::node_id();
        
        // Se o token é para este node ou para "border" (vindo do gateway via UART)
        if (packet.route.dst == my_id || packet.route.dst == "border")
        {
            // Se destino é "border" e este é o border node, processa localmente
            if (packet.route.dst == "border")
            {
                ESP_LOGI(TAG, "TOKEN recebido do Gateway via UART: %s -> border (este node)", packet.route.src.c_str());
                // Ajusta origem para "gateway" para o test_packet_generator saber que veio do gateway
                Protocol::Packet adjusted_packet = packet;
                adjusted_packet.route.dst = my_id; // Ajusta destino para este node
                on_token_received(adjusted_packet);
                return;
            }
            else
            {
                ESP_LOGI(TAG, "TOKEN recebido: %s -> %s (este node)", packet.route.src.c_str(), packet.route.dst.c_str());
                // Notifica o test_packet_generator
                on_token_received(packet);
                return;
            }
        }
        // Se é broadcast, qualquer node pode receber (mas só processa se não tiver token)
        else if (packet.route.dst == "broadcast")
        {
            ESP_LOGI(TAG, "TOKEN broadcast recebido de %s", packet.route.src.c_str());
            on_token_received(packet);
            return;
        }
        // Se não é para este node, reencaminha pela mesh
        else
        {
            ESP_LOGI(TAG, "TOKEN não é para este node (%s), reencaminhando para %s", 
                     my_id.c_str(), packet.route.dst.c_str());
            ESPNOWTransport::send(packet);
            return;
        }
    }
#endif
    
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

void NetworkManager::on_hello(const std::string &node_id, int rssi)
{
    const uint64_t now = now_ms();
    for (auto &n : s_neighbors)
    {
        if (n.id == node_id)
        {
            n.last_seen_ms = now;
            n.rssi = rssi;
            return;
        }
    }
    s_neighbors.push_back({node_id, rssi, now});
}
