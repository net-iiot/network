#include <string>
#include "esp_log.h"
#include "esp_mac.h" // ✅ necessário agora
#include "esp_system.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "protocol.hpp"
#include "router.hpp"
#include "gateway.hpp"
#include "ble_transport.hpp"

static const char *TAG = "APP";

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "   WetzelMesh - Sistema de Comunicação Mesh  ");
    ESP_LOGI(TAG, "============================================");

    ESP_LOGI(TAG, "Inicializando Router...");
    WetzelMesh::Router::init();

    ESP_LOGI(TAG, "Inicializando Gateway UART...");
    WetzelMesh::Gateway::init();

    ESP_LOGI(TAG, "Inicializando BLE Mesh Transport...");
    WetzelMesh::BLETransport::init();

    WetzelMesh::Protocol::Packet pkt;
    pkt.type = WetzelMesh::Protocol::PacketType::REQUEST;
    pkt.route.src = "node-01";
    pkt.route.dst = "gateway";
    pkt.method = "POST";
    pkt.endpoint = "/api/telemetry";
    pkt.body = R"({"temperature": 24.5, "humidity": 62, "voltage": 3.78})";

    ESP_LOGI(TAG, "Enviando pacote inicial de teste...");
    WetzelMesh::Gateway::send(pkt);
    WetzelMesh::BLETransport::send(pkt);

    ESP_LOGI(TAG, "Sistema WetzelMesh inicializado com sucesso!");
    ESP_LOGI(TAG, "Aguardando pacotes da rede...");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
