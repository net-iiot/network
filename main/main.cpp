#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "led_manager.hpp"
#include "protocol.hpp"
#include "router.hpp"
#include "network_manager.hpp"
#include "test_packet_generator.hpp"
#include "gateway.hpp" // ainda útil para o modo gateway, mas init é feito no NetworkManager

using namespace WetzelMesh;

static const char *TAG = "WETZELMESH";

extern "C" void app_main(void)
{
    // Escolha do modo
#ifdef CONFIG_WETZEL_IS_GATEWAY
    constexpr bool kIsGateway = true;
#else
    constexpr bool kIsGateway = false;
#endif

    ESP_LOGI(TAG, "Iniciando WetzelMesh (%s)", kIsGateway ? "Gateway" : "Node");

    // Inicializa NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicializa subsistemas
    LedManager::init(kIsGateway);
    ESP_LOGI(TAG, "LED Manager inicializado");

    Router::init(kIsGateway);
    NetworkManager::init(kIsGateway); // aqui dentro o Gateway::init() é chamado se for gateway

    // Geração de pacotes de teste (opcional)
    start_test_generation();

    ESP_LOGI(TAG, "WetzelMesh pronto no modo: %s", kIsGateway ? "Gateway" : "Node");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
