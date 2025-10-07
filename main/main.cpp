#include <iostream>
#include <memory>
extern "C" {
    #include "esp_log.h"
    #include "nvs_flash.h"
}

#include "mesh_transport.hpp"
#include "uart_bridge.hpp"
#include "mqtt_gateway.hpp"
#include "interpreter_registry.hpp"

static const char* TAG = "APP";

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "monimesh boot OK (C++ project)");

    monimesh::MeshTransport mesh;
    monimesh::UARTBridge uart;
    monimesh::MQTTGateway mqtt;

    mesh.init();
    uart.init();
    mqtt.connect();

    ESP_LOGI(TAG, "System initialized");
}
