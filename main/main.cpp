#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "led_manager.hpp"
#include "protocol.hpp"
#include "router.hpp"
#include "network_manager.hpp"

using namespace WetzelMesh;

static const char *TAG = "WETZELMESH";

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    const bool isGateway = false;

    LedManager::init(isGateway);
    ESP_LOGI(TAG, "LED Manager inicializado");

    Router::init();
    NetworkManager::init(isGateway);

    ESP_LOGI(TAG, "WetzelMesh pronto: modo %s", isGateway ? "Gateway" : "Node");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
