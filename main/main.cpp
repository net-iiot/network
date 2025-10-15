#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "led_manager.hpp"
#include "protocol.hpp"
#include "router.hpp"
#include "network_manager.hpp"
#include "test_packet_generator.hpp"
#include "gateway.hpp"

using namespace WetzelMesh;

static const char *TAG = "WETZELMESH";

extern "C" void app_main(void)
{
    // Papel (defina em menuconfig: Wetzel Mesh -> Este build é GATEWAY)
#ifdef CONFIG_WETZEL_IS_GATEWAY
    constexpr bool kIsGateway = true;
#else
    constexpr bool kIsGateway = false;
#endif

    // Logs mais verbosos nos nossos módulos
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("GATEWAY", ESP_LOG_INFO);
    esp_log_level_set("NETMAN", ESP_LOG_INFO);
    esp_log_level_set("BORDER_UART", ESP_LOG_INFO);
    esp_log_level_set("ESPNOW", ESP_LOG_INFO); // no gateway NÃO deve aparecer "ESPNOW iniciado"

    ESP_LOGI(TAG, "Iniciando WetzelMesh (%s)", kIsGateway ? "Gateway" : "Node");

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Base de rede/eventos (seguro chamar sempre)
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    // Subsistemas
    LedManager::init(kIsGateway);
    ESP_LOGI(TAG, "LED Manager inicializado");

    Router::init(kIsGateway);
    NetworkManager::init(kIsGateway); // se gateway: BLE/ESPNOW OFF, só Gateway::init()

    // Geração de pacotes de teste a cada 1s (mantive seu nome de função)
    // Gateway: gera e envia via UART -> borda -> mesh
    // Node: gera e envia via mesh
    start_test_generation();

    ESP_LOGI(TAG, "WetzelMesh pronto no modo: %s", kIsGateway ? "Gateway" : "Node");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
