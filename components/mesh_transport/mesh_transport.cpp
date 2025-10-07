#include "mesh_transport.hpp"
#include "esp_log.h"
#include "uart_bridge.hpp"

using wetzelmesh::UARTBridge;

namespace wetzelmesh {

MeshTransport::MeshTransport() : rxHandler_(nullptr) {}

esp_err_t MeshTransport::init() {
    ESP_LOGI(TAG, "Inicializando transporte Mesh (UART)");
    static UARTBridge uart;
    uart.init();

    uart.setPacketHandler([this](const std::vector<uint8_t>& data) {
        std::string json(data.begin(), data.end());
        if (rxHandler_) rxHandler_(json);
    });

    ESP_LOGI(TAG, "UART Bridge conectado ao transporte");
    return ESP_OK;
}

void MeshTransport::setRxHandler(RxHandler handler) {
    rxHandler_ = std::move(handler);
}

esp_err_t MeshTransport::send(const std::string& json) {
    std::vector<uint8_t> bytes(json.begin(), json.end());
    ESP_LOGI(TAG, "Enviando pacote (%d bytes)", (int)bytes.size());
    UARTBridge uart;
    uart.sendPacket(bytes);
    return ESP_OK;
}

void MeshTransport::simulateRx(const std::string& json) {
    ESP_LOGI(TAG, "Simulação RX: %s", json.c_str());
    if (rxHandler_) rxHandler_(json);
}

} // namespace wetzelmesh
