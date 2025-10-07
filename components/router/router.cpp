#include "router.hpp"
#include "esp_log.h"
#include "gateway.hpp"
#include "ble_transport.hpp"
#include "network_manager.hpp"

using namespace WetzelMesh;
static const char *TAG = "ROUTER";

void Router::init()
{
    ESP_LOGI(TAG, "Router iniciado com sucesso.");
}

void Router::handle_packet(const Protocol::Packet &packet)
{
    ESP_LOGI(TAG, "📦 Recebido pacote de %s → %s (%s)",
             packet.route.src.c_str(),
             packet.route.dst.c_str(),
             packet.endpoint.c_str());

    // Se o destino é o gateway → envia via UART
    if (packet.route.dst == "gateway")
    {
        ESP_LOGI(TAG, "🔁 Encaminhando para gateway via UART...");
        Gateway::send(packet);
        return;
    }

    // Se o destino é broadcast → envia para todos os vizinhos BLE
    if (packet.route.dst == "broadcast")
    {
        ESP_LOGI(TAG, "📡 Broadcast para rede BLE...");
        NetworkManager::broadcast(packet);
        return;
    }

    // Se é outro nó BLE específico
    if (packet.route.dst.find("node-") == 0)
    {
        ESP_LOGI(TAG, "➡️ Encaminhando via BLE para %s", packet.route.dst.c_str());
        BLETransport::send(packet);
        return;
    }

    // Se é local → processar (por enquanto só loga)
    ESP_LOGI(TAG, "📍 Pacote destinado a este nó, processando localmente...");
}

void Router::send_to(const Protocol::Packet &packet, const std::string &target)
{
    Protocol::Packet p = packet;
    p.route.dst = target;
    handle_packet(p);
}
