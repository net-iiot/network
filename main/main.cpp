#include <string>
#include "esp_log.h"
#include "json_codec.hpp"       // substitui o antigo cbor_codec.hpp
#include "mesh_transport.hpp"   // seu módulo de transporte
#include "uart_bridge.hpp"      // seu módulo UART
#include "mqtt_gateway.hpp"     // gateway MQTT

static const char* TAG = "APP";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "wetzelmesh boot OK (C++ project)");

    // Inicializações placeholder
    ESP_LOGI(TAG, "MeshTransport: MeshTransport init (placeholder)");
    ESP_LOGI(TAG, "UARTBridge: UARTBridge initialized (placeholder)");
    ESP_LOGI(TAG, "MQTTGateway: Connecting to MQTT broker (placeholder)");

    // Exemplo de uso do codec JSON
    std::string encoded = wetzelmesh::JSONCodec::encode("status", "mesh node online");
    ESP_LOGI(TAG, "Encoded message: %s", encoded.c_str());

    std::string type, payload;
    if (wetzelmesh::JSONCodec::decode(encoded, type, payload)) {
        ESP_LOGI(TAG, "Decoded message -> type: %s | payload: %s", type.c_str(), payload.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to decode JSON message");
    }

    ESP_LOGI(TAG, "APP: System initialized");
}
