#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <cstring>

#include "led_manager.hpp"
#include "protocol.hpp"
#include "router.hpp"
#include "network_manager.hpp"
#include "test_packet_generator.hpp"
#include "gateway.hpp"
#include "network_mapper.hpp"
#include "ota_manager.hpp"

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

    // Configura níveis de log ANTES de qualquer log
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("WETZELMESH", ESP_LOG_INFO);
    esp_log_level_set("GATEWAY", ESP_LOG_INFO);
    esp_log_level_set("BORDER_UART", ESP_LOG_INFO);
    esp_log_level_set("NETMAN", ESP_LOG_INFO);
    esp_log_level_set("ESPNOW", ESP_LOG_INFO);
    
    // Pequeno delay para garantir que o monitor está pronto
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "INICIANDO WETZEL MESH");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "Modo: %s", kIsGateway ? "Gateway" : "Node");
    
    // Mostra configurações do menuconfig (se Gateway)
    if (kIsGateway)
    {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "CONFIGURAÇÕES DO MENUCONFIG (Gateway):");
#ifdef CONFIG_WETZEL_GATEWAY_WIFI_SSID
        ESP_LOGI(TAG, "   WiFi SSID: '%s'", CONFIG_WETZEL_GATEWAY_WIFI_SSID);
#else
        ESP_LOGW(TAG, "   WiFi SSID: NÃO CONFIGURADO!");
#endif
#ifdef CONFIG_WETZEL_GATEWAY_WIFI_PASSWORD
        const char* pwd = CONFIG_WETZEL_GATEWAY_WIFI_PASSWORD;
        ESP_LOGI(TAG, "   WiFi Password: %s", 
                 (pwd && strlen(pwd) > 0) ? "*** (configurada)" : "(vazia - rede aberta)");
#else
        ESP_LOGW(TAG, "   WiFi Password: NÃO CONFIGURADO!");
#endif
#ifdef CONFIG_WETZEL_GATEWAY_SERVER_URL
        ESP_LOGI(TAG, "   Server URL: '%s'", CONFIG_WETZEL_GATEWAY_SERVER_URL);
#else
        ESP_LOGW(TAG, "   Server URL: NÃO CONFIGURADO!");
#endif
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    }

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
    
    // Inicializa Gateway com URL do servidor (se gateway)
    if (kIsGateway)
    {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Chamando Gateway::init()...");
#ifdef CONFIG_WETZEL_GATEWAY_SERVER_URL
        Gateway::init(CONFIG_WETZEL_GATEWAY_SERVER_URL);
#else
        Gateway::init("http://192.168.1.100:8080"); // Fallback se constante não existir
#endif
        ESP_LOGI(TAG, "Gateway::init() concluído!");
    }
    
    NetworkManager::init(kIsGateway); // se gateway: BLE/ESPNOW OFF, só Gateway::init()
    
    // Inicializa mapeador de rede (apenas gateway faz mapeamento ativo)
    NetworkMapper::init(kIsGateway);
    if (kIsGateway)
    {
        // Configura mapeamento periódico a cada 60 segundos (opcional)
        // NetworkMapper::set_periodic_mapping(60);
        ESP_LOGI(TAG, "NetworkMapper configurado no gateway");
    }
    
    // Inicializa OTA Manager
#ifdef CONFIG_WETZEL_GATEWAY_SERVER_URL
    OTAManager::init(kIsGateway, CONFIG_WETZEL_GATEWAY_SERVER_URL);
#else
    OTAManager::init(kIsGateway, "");
#endif
    if (kIsGateway)
    {
        ESP_LOGI(TAG, "OTA Manager configurado no gateway");
    }

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
