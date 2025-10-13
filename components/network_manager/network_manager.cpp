#include "network_manager.hpp"
#include "esp_log.h"
#include "ble_transport.hpp"
#include "gateway.hpp"
#include "router.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_manager.hpp"
#include "espnow_transport.hpp"

using namespace WetzelMesh;

static const char *TAG = "NETMAN";

bool NetworkManager::s_gateway = false;
std::vector<Neighbor> NetworkManager::s_neighbors;

void NetworkManager::init(bool isGateway)
{
    s_gateway = isGateway;

    BLETransport::init(isGateway);
    if (s_gateway)
        Gateway::init();

    xTaskCreatePinnedToCore(&NetworkManager::refresh_neighbors_task, "neighbors", 4096, nullptr, 4, nullptr, 0);

    ESP_LOGI(TAG, "Network Manager inicializado (%s)", isGateway ? "Gateway" : "Node");
}

void NetworkManager::refresh_neighbors_task(void *param)
{
    // Stub de descoberta — numa malha real, populamos a partir de scan/conexões
    for (;;)
    {
        s_neighbors.clear();
        s_neighbors.push_back({BLETransport::node_id(), -40});
        vTaskDelay(pdMS_TO_TICKS(3000));
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
    // Por ora, flooding simples na mesh
    return ESPNOWTransport::send(p);
}

void NetworkManager::handle_incoming(const Protocol::Packet &packet)
{
    ESP_LOGI("NETMAN", "Pacote recebido de %s para %s",
             packet.route.src.c_str(),
             packet.route.dst.c_str());
    LedManager::on_packet_received();
    vTaskDelay(pdMS_TO_TICKS(1000));
    NetworkManager::send(packet);
}
