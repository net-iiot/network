#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include <cstring>

#include "led_manager.hpp"
#include "protocol.hpp"
#include "network_manager.hpp"
#include "gateway.hpp"
#include "network_mapper.hpp"
#include "ota_manager.hpp"

using namespace NetworkMesh;

static const char *TAG = "NETWORKMESH";

extern "C" void app_main(void)
{
#ifdef CONFIG_NETWORK_IS_GATEWAY
    constexpr bool kIsGateway = true;
#else
    constexpr bool kIsGateway = false;
#endif

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("NETWORKMESH", ESP_LOG_INFO);
    esp_log_level_set("GATEWAY", ESP_LOG_INFO);
    esp_log_level_set("BORDER_UART", ESP_LOG_INFO);
    esp_log_level_set("NETMAN", ESP_LOG_INFO);
    esp_log_level_set("ESPNOW", ESP_LOG_INFO);
    esp_log_level_set("BLE", ESP_LOG_INFO);

    vTaskDelay(pdMS_TO_TICKS(500));

    uint8_t mac[6];
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "INICIANDO REDE");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    if (mac_err == ESP_OK)
    {
        ESP_LOGI(TAG, "MAC ADDRESS: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    ESP_LOGI(TAG, "Modo: %s", kIsGateway ? "Gateway" : "Node");

    if (kIsGateway)
    {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "CONFIGURACOES DO MENUCONFIG (Gateway):");
#ifdef CONFIG_NETWORK_GATEWAY_WIFI_SSID
        ESP_LOGI(TAG, "   WiFi SSID: '%s'", CONFIG_NETWORK_GATEWAY_WIFI_SSID);
#else
        ESP_LOGW(TAG, "   WiFi SSID: NAO CONFIGURADO!");
#endif
#ifdef CONFIG_NETWORK_GATEWAY_WIFI_PASSWORD
        const char *pwd = CONFIG_NETWORK_GATEWAY_WIFI_PASSWORD;
        ESP_LOGI(TAG, "   WiFi Password: %s",
                 (pwd && strlen(pwd) > 0) ? "*** (configurada)" : "(vazia - rede aberta)");
#else
        ESP_LOGW(TAG, "   WiFi Password: NAO CONFIGURADO!");
#endif
#ifdef CONFIG_NETWORK_GATEWAY_SERVER_URL
        ESP_LOGI(TAG, "   Server URL: '%s'", CONFIG_NETWORK_GATEWAY_SERVER_URL);
#else
        ESP_LOGW(TAG, "   Server URL: NAO CONFIGURADO!");
#endif
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    LedManager::init(kIsGateway);
    ESP_LOGI(TAG, "LED Manager inicializado");

    if (kIsGateway)
    {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Chamando Gateway::init()...");
#ifdef CONFIG_NETWORK_GATEWAY_SERVER_URL
        Gateway::init(CONFIG_NETWORK_GATEWAY_SERVER_URL);
#else
        Gateway::init("http://example.com/api");
#endif
        ESP_LOGI(TAG, "Gateway::init() concluido!");
    }

    NetworkManager::init(kIsGateway);

    NetworkMapper::init(kIsGateway);
    if (kIsGateway)
    {
        NetworkMapper::set_periodic_mapping(0);
        ESP_LOGI(TAG, "NetworkMapper configurado no gateway (mapeamento por eventos apenas)");
    }

#ifdef CONFIG_NETWORK_GATEWAY_SERVER_URL
    OTAManager::init(kIsGateway, CONFIG_NETWORK_GATEWAY_SERVER_URL);
#else
    OTAManager::init(kIsGateway, "");
#endif
    if (kIsGateway)
        ESP_LOGI(TAG, "OTA Manager configurado no gateway");

    ESP_LOGI(TAG, "Rede pronta no modo: %s", kIsGateway ? "Gateway" : "Node");

    while (true)
        vTaskDelay(pdMS_TO_TICKS(5000));
}
