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
        Gateway::init(); // somente UART+server
        ESP_LOGI(TAG, "INIT: GATEWAY (mesh/BLE OFF)");
    }
    else
    {
        ESPNOWTransport::init();
        BLETransport::init(false);

        BorderUart::init();
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
    LedManager::on_packet_received();

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
        return;

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

            ESP_LOGI(TAG, "TX[HELLO] %s -> broadcast", hello.route.src.c_str());
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
