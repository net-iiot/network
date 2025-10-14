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

uint64_t NetworkManager::now_ms()
{
    return esp_timer_get_time() / 1000ULL;
}

void NetworkManager::init(bool isGateway)
{
    s_gateway = isGateway;

    if (s_gateway)
    {
        // GATEWAY: ponte com servidor + UART com nó-borda (sem mesh/ble)
        Gateway::init();
        ESP_LOGI(TAG, "Network Manager: modo Gateway (mesh/BLE desativados)");
    }
    else
    {
        // NODE: ESPNOW + BLE + Border UART (se este for o nó-borda)
        ESPNOWTransport::init();
        BLETransport::init(false);

        // Inicializa a UART da borda (se não houver cabos, apenas não trafega).
        BorderUart::init();

        // Registra handler: tudo que vier da UART do gateway entra no roteador
        BorderUart::set_rx_handler([](const Protocol::Packet &pkt)
                                   { Router::handle_packet(pkt); });

        xTaskCreatePinnedToCore(&NetworkManager::refresh_neighbors_task, "neighbors", 4096, nullptr, 4, nullptr, 0);
        start_hello_task();
        ESP_LOGI(TAG, "Network Manager: modo Node (mesh ESPNOW + BLE + (opcional) Border UART)");
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

bool NetworkManager::is_gateway()
{
    return s_gateway;
}

const std::vector<Neighbor> &NetworkManager::neighbors()
{
    return s_neighbors;
}

bool NetworkManager::send(const Protocol::Packet &p)
{
    if (s_gateway)
    {
        // Gateway: sempre envia ao nó-borda via UART (ele injeta na mesh)
        return Gateway::send_to_border(p);
    }
    else
    {
        // Nó:
        // Se o destino é o gateway e este nó é a borda (UART ativa), sobe por UART.
        if (p.route.dst == "gateway" && BorderUart::is_enabled())
            return BorderUart::send_to_gateway(p);

        // Caso contrário, envia pela mesh ESPNOW
        return ESPNOWTransport::send(p);
    }
}

void NetworkManager::handle_incoming(const Protocol::Packet &packet)
{
    ESP_LOGI("NETMAN", "RX %s -> %s", packet.route.src.c_str(), packet.route.dst.c_str());
    LedManager::on_packet_received();

    if (s_gateway)
    {
        if (packet.route.dst == "server")
            Gateway::send(packet);
        else
            Gateway::send_to_border(packet);
        return;
    }

    // Em nó:
    if (packet.type == Protocol::PacketType::EVENT && packet.method == "HELLO")
        return; // não floodar HELLO

    if (packet.route.dst == "gateway")
    {
        // encaminhamento “para cima” — mesma regra do send()
        if (BorderUart::is_enabled())
            BorderUart::send_to_gateway(packet);
        else
            ESPNOWTransport::send(packet);
        return;
    }

    // Broadcast/controlado — comente/descomente conforme a necessidade
    // if (packet.route.dst == "broadcast") ESPNOWTransport::send(packet);
}

void NetworkManager::start_hello_task()
{
    auto hello_task = [](void *)
    {
        vTaskDelay(pdMS_TO_TICKS(400)); // aguarda mesh estabilizar
        for (;;)
        {
            Protocol::Packet hello{};
            hello.type = Protocol::PacketType::EVENT;
            hello.method = "HELLO";
            hello.route.src = BLETransport::node_id();
            hello.route.dst = "broadcast";
            hello.body = R"({"t":"hello"})";

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
