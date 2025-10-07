#include "router.hpp"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace WetzelMesh;
static const char *TAG = "ROUTER";

void Router::init()
{
    ESP_LOGI(TAG, "Router iniciado.");
}

void Router::handle_packet(const Protocol::Packet &packet)
{
    ESP_LOGI(TAG, "Roteando pacote de %s -> %s",
             packet.route.src.c_str(), packet.route.dst.c_str());
    ESP_LOGI(TAG, "Endpoint: %s", packet.endpoint.c_str());

    if (packet.route.dst == "gateway")
    {
        send_to_gateway(packet);
    }
    else if (packet.route.dst.starts_with("node-"))
    {
        send_to_ble(packet);
    }
    else if (packet.route.dst == "uart")
    {
        send_to_uart(packet);
    }
    else
    {
        ESP_LOGW(TAG, "Destino desconhecido: %s", packet.route.dst.c_str());
    }
}

void Router::send_to_gateway(const Protocol::Packet &packet)
{
    std::string json = Protocol::serialize(packet);
    ESP_LOGI(TAG, "→ Enviando ao Gateway: %s", json.c_str());
    // Aqui futuramente: envio via UART
}

void Router::send_to_ble(const Protocol::Packet &packet)
{
    std::string json = Protocol::serialize(packet);
    ESP_LOGI(TAG, "→ Enviando via BLE Mesh: %s", json.c_str());
    // Aqui futuramente: envio via BLE
}

void Router::send_to_uart(const Protocol::Packet &packet)
{
    std::string json = Protocol::serialize(packet);
    ESP_LOGI(TAG, "→ Enviando via UART: %s", json.c_str());
    // Aqui futuramente: escrita no UART TX
}
