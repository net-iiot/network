#include <iostream>
#include <memory>
extern "C" {
    #include "esp_log.h"
    #include "nvs_flash.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "esp_system.h"
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

#if CONFIG_APP_ROLE_GATEWAY
    ESP_LOGI(TAG, "Device Role: GATEWAY");
#elif CONFIG_APP_ROLE_NODE
    ESP_LOGI(TAG, "Device Role: NODE");
#else
    #error "Device role not defined! Check menuconfig."
#endif

    monimesh::MeshTransport mesh;
    monimesh::UARTBridge uart;
    monimesh::MQTTGateway mqtt;

    ESP_ERROR_CHECK(mesh.init());
    ESP_ERROR_CHECK(uart.init());
#if CONFIG_APP_ROLE_GATEWAY
    ESP_ERROR_CHECK(mqtt.connect());
#endif

    ESP_LOGI(TAG, "System initialized");

    while (true) {
        mesh.process();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
