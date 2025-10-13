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

    // 1) ESPNOW: base da malha (connectionless)
    ESPNOWTransport::init();

    // 2) BLE: visibilidade/app (opcional)
    BLETransport::init(isGateway);

    // 3) Se for gateway, inicia ponte (Wi-Fi/HTTP/UART)
    if (s_gateway)
        Gateway::init();

    // 4) Manutenção de vizinhos (expurgo e LED de join)
    xTaskCreatePinnedToCore(&NetworkManager::refresh_neighbors_task, "neighbors", 4096, nullptr, 4, nullptr, 0);

    // 5) Anúncio HELLO periódico (descoberta automática)
    start_hello_task();

    ESP_LOGI(TAG, "Network Manager inicializado (%s)", isGateway ? "Gateway" : "Node");
}

void NetworkManager::refresh_neighbors_task(void *param)
{
    for (;;)
    {
        const uint64_t now = now_ms();
        bool any_recent = false;

        for (auto it = s_neighbors.begin(); it != s_neighbors.end();)
        {
            if ((now - it->last_seen_ms) > 8000) // 8s sem HELLO -> remove
            {
                it = s_neighbors.erase(it);
            }
            else
            {
                any_recent = true;
                ++it;
            }
        }

        // Node indica "joined" se houver ao menos 1 vizinho recente
        if (!s_gateway)
        {
            LedManager::set_node_joined(any_recent);
        }

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
    // Flood simples (broadcast) ou unicast, conforme o Packet::route.dst que você usar.
    return ESPNOWTransport::send(p);
}

void NetworkManager::handle_incoming(const Protocol::Packet &packet)
{
    ESP_LOGI("NETMAN", "RX %s -> %s", packet.route.src.c_str(), packet.route.dst.c_str());
    LedManager::on_packet_received();

    // Evitar loops: não reencaminhar HELLO nem rebroadcast cego
    if (packet.route.dst == "broadcast")
    {
        if (packet.type == Protocol::PacketType::EVENT && packet.method == "HELLO")
        {
            // HELLO só atualiza vizinhança; não reflooda
            return;
        }

        // (Se quiser propagar outros eventos de broadcast, descomente)
        // NetworkManager::send(packet);
        return;
    }
}

void NetworkManager::start_hello_task()
{
    auto hello_task = [](void *)
    {
        // Aguarda ESPNOW estabilizar (peer e canal) antes do 1º HELLO
        vTaskDelay(pdMS_TO_TICKS(400));

        for (;;)
        {
            Protocol::Packet hello{};
            hello.type = Protocol::PacketType::EVENT;
            hello.method = "HELLO";
            hello.route.src = BLETransport::node_id();
            hello.route.dst = "broadcast";
            hello.body = R"({"t":"hello"})";

            ESPNOWTransport::send(hello);
            vTaskDelay(pdMS_TO_TICKS(2000)); // HELLO a cada 2s
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
