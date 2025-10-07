#include "network_manager.hpp"
#include "esp_log.h"
#include "ble_transport.hpp"
#include "gateway.hpp"
#include "router.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace WetzelMesh;

static const char *TAG = "NETMAN";
std::vector<Neighbor> NetworkManager::neighbors;
bool NetworkManager::gatewayMode = false;

void NetworkManager::init(bool isGateway) {
    gatewayMode = isGateway;
    ESP_LOGI(TAG, "Network Manager inicializado (%s)", isGateway ? "Gateway" : "Node");
    ESP_LOGI(TAG, "Inicializando Network Manager...");
    scan_neighbors();

    // Aqui você pode criar uma task futura para atualizar a tabela periodicamente
    ESP_LOGI(TAG, "Network Manager inicializado.");
}

void NetworkManager::scan_neighbors()
{
    // Aqui simulamos vizinhos descobertos na malha BLE
    neighbors.clear();
    neighbors.push_back({"node-02", -55});
    neighbors.push_back({"node-03", -60});
    ESP_LOGI(TAG, "Nós vizinhos detectados: %d", (int)neighbors.size());
}

void NetworkManager::broadcast(const Protocol::Packet &packet)
{
    ESP_LOGI(TAG, "🔊 Broadcast para %d vizinhos", (int)neighbors.size());
    for (auto &n : neighbors)
    {
        ESP_LOGI(TAG, "→ Enviando pacote para %s via BLE", n.id.c_str());
        BLETransport::send(packet);
    }
}

void NetworkManager::handle_incoming(const Protocol::Packet &packet)
{
    ESP_LOGI(TAG, "📥 Pacote recebido de %s", packet.route.src.c_str());
    Router::handle_packet(packet);
}

const std::vector<Neighbor> &NetworkManager::get_neighbors()
{
    return neighbors;
}
