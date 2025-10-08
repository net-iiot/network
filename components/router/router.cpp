#include "router.hpp"
#include "esp_log.h"
#include "gateway.hpp"
#include "ble_transport.hpp"
#include "network_manager.hpp"
#include "led_manager.hpp"

using namespace WetzelMesh;

static const char *TAG = "ROUTER";

void Router::init()
{
    ESP_LOGI(TAG, "Router iniciado");
}

void Router::handle_packet(const Protocol::Packet &packet)
{
    // Atualiza LED de tráfego
    LedManager::blink();

    const bool is_gateway = NetworkManager::is_gateway();
    const std::string &dst = packet.route.dst;

    // Se sou o destino "gateway" e sou gateway -> UART/servidor
    if (dst == "gateway" && is_gateway)
    {
        ESP_LOGI(TAG, "Destino é o GATEWAY local → UART");
        Gateway::send(packet);
        return;
    }

    // Se destino for outro nó/flutter → retransmite por BLE
    if (dst == "flutter" || dst.rfind("node-", 0) == 0)
    {
        ESP_LOGI(TAG, "Encaminhando por BLE para destino: %s", dst.c_str());
        BLETransport::send(packet);
        return;
    }

    // Caso contrário, se sou node e destino é gateway → BLE
    if (!is_gateway && dst == "gateway")
    {
        ESP_LOGI(TAG, "Sou node e destino é o gateway → BLE");
        BLETransport::send(packet);
        return;
    }

    // Pacote local (não há outro destino claro) — por enquanto apenas loga
    ESP_LOGI(TAG, "📍 Pacote para processamento local: %s %s", packet.method.c_str(), packet.endpoint.c_str());
}

void Router::send_to(const Protocol::Packet &packet, const std::string &target)
{
    Protocol::Packet p = packet;
    p.route.dst = target;
    handle_packet(p);
}
