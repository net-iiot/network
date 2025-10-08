#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "led_manager.hpp"
#include "protocol.hpp"
#include "router.hpp"
#include "network_manager.hpp"
#include "test_packet_generator.hpp"

using namespace WetzelMesh;

static const char *TAG = "WETZELMESH";
static constexpr bool kIsGateway = false;

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    LedManager::init(kIsGateway);
    ESP_LOGI(TAG, "LED Manager inicializado");

    Router::init();
    NetworkManager::init(kIsGateway);
    WetzelMesh::start_test_generation();


    ESP_LOGI(TAG, "WetzelMesh pronto: modo %s", kIsGateway ? "Gateway" : "Node");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
