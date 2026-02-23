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
#include "router.hpp"
#include "network_manager.hpp"
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
    esp_log_level_set("BLE", ESP_LOG_INFO);
    
    // Delay para garantir que o monitor está pronto
    vTaskDelay(pdMS_TO_TICKS(500));

    // Lê e exibe MAC address IMEDIATAMENTE (antes de qualquer inicialização)
    uint8_t mac[6];
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (mac_err == ESP_OK)
    {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "INICIANDO WETZEL MESH");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "MAC ADDRESS: %02X:%02X:%02X:%02X:%02X:%02X", 
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Modo: %s", kIsGateway ? "Gateway" : "Node");
    }
    else
    {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "INICIANDO WETZEL MESH");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "Modo: %s", kIsGateway ? "Gateway" : "Node");
        ESP_LOGW(TAG, "Erro ao ler MAC address: %s", esp_err_to_name(mac_err));
    }
    
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
        Gateway::init("http://172.16.10.110:5002"); // Fallback se constante não existir
#endif
        ESP_LOGI(TAG, "Gateway::init() concluído!");
    }
    
    NetworkManager::init(kIsGateway); // se gateway: BLE/ESPNOW OFF, só Gateway::init()
    
    // Inicializa mapeador de rede (apenas gateway faz mapeamento ativo)
    NetworkMapper::init(kIsGateway);
    if (kIsGateway)
    {
        // ✅ MUDANÇA: Mapeamento agora é apenas por eventos (NODE_JOINED/NODE_LEFT)
        // Não há mais polling periódico - mapeamento é disparado quando:
        // 1. Novo node é detectado via HELLO (notificado pelo border node)
        // 2. Node é removido por timeout (notificado pelo border node)
        // 3. Mapeamento inicial após 5 segundos (já implementado no NetworkMapper::init)
        NetworkMapper::set_periodic_mapping(0); // 0 = desabilitado (apenas eventos)
        
        ESP_LOGI(TAG, "NetworkMapper configurado no gateway (mapeamento por eventos apenas)");
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

    ESP_LOGI(TAG, "WetzelMesh pronto no modo: %s", kIsGateway ? "Gateway" : "Node");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
