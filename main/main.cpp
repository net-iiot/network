#include <string>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
// #include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "protocol.hpp"
#include "router.hpp"
#include "gateway.hpp"

static const char *TAG = "APP";

/* ======================================================
 *  FUNÇÃO PRINCIPAL - WETZELMESH
 * ====================================================== */
extern "C" void app_main(void)
{
    // ======================================================
    // 🔧 Inicialização do sistema
    // ======================================================
    // esp_err_t ret = nvs_flash_init();
    // if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    // {
    //     nvs_flash_erase();
    //     nvs_flash_init();
    // }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " WETZELMESH - Sistema inicializando...");
    ESP_LOGI(TAG, "================================================");

    // ======================================================
    // 🧠 Inicialização dos módulos principais
    // ======================================================
    ESP_LOGI(TAG, "Inicializando protocolo JSON...");
    auto pkt = Protocol::make_request(
        "node-01",
        "gateway",
        "POST",
        "/api/telemetry",
        R"({"temperature":25.4,"humidity":61,"voltage":3.79})");

    std::string jsonStr = Protocol::serialize(pkt);
    ESP_LOGI(TAG, "Pacote serializado: %s", jsonStr.c_str());

    Protocol::Packet parsed;
    if (Protocol::parse(jsonStr, parsed))
    {
        ESP_LOGI(TAG, "Pacote analisado com sucesso:");
        ESP_LOGI(TAG, "  Type: %d", static_cast<int>(parsed.type));
        ESP_LOGI(TAG, "  Route: %s -> %s", parsed.route.src.c_str(), parsed.route.dst.c_str());
        ESP_LOGI(TAG, "  Endpoint: %s", parsed.endpoint.c_str());
        ESP_LOGI(TAG, "  Body: %s", parsed.body.c_str());
    }
    else
    {
        ESP_LOGE(TAG, "Falha ao interpretar pacote JSON!");
    }

    // ======================================================
    // 🚦 Inicialização do roteador
    // ======================================================
    ESP_LOGI(TAG, "Inicializando roteador...");
    WetzelMesh::Router::init();

    // Teste de roteamento
    ESP_LOGI(TAG, "Enviando pacote de teste ao roteador...");
    WetzelMesh::Router::handle_packet(pkt);

    ESP_LOGI(TAG, "Inicializando gateway UART...");
    WetzelMesh::Gateway::init();
    // Gateway::start_http_loop();
    // BLETransport::start_mesh();

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " Sistema WetzelMesh inicializado com sucesso!");
    ESP_LOGI(TAG, "================================================");

    WetzelMesh::Gateway::send(pkt);

    // ======================================================
    // 🕒 Loop principal (placeholder)
    // ======================================================
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "WetzelMesh em execução...");
    }
}
