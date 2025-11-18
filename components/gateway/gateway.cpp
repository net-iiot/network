#include "gateway.hpp"
#include "http_context.hpp"
#include "esp_log.h"
#include "driver/uart.h"
#include "protocol.hpp"
#include "network_manager.hpp"
#include "esp_bt.h"
#include "led_manager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <string>
#include <queue>
#include <mutex>
#include <cstring>
#include <cassert>
#include <cstdlib>
#include <ctime>

namespace WetzelMesh
{

    static const char *TAG = "GATEWAY";

    static constexpr uart_port_t kUartNum = UART_NUM_1;
    static constexpr int kTxPin = 17; // Teste: TX -> RX do nó-borda (GPIO15)
    static constexpr int kRxPin = 16; // Teste: RX <- TX do nó-borda (GPIO13)
    static constexpr int kBaud = 115200;
    static constexpr size_t kBufSize = 2048;
    static constexpr size_t kMaxChunkSize = 1500; // Tamanho máximo de chunk
    
    // URL do servidor
    std::string Gateway::s_server_url = "http://localhost:8080";
    
    // Fila de requisições HTTP pendentes
    static std::queue<Protocol::Packet> s_http_request_queue;
    static std::mutex s_queue_mutex;

    // Só a test task permanece como função livre
    static void test_packet_task(void *);
    static void uart_status_task(void *);
    static void wifi_reconnect_task(void *);
    static void wifi_scan_task(void *);
    
    // Estado do token no Gateway
    static bool s_gateway_has_token = false;
    
    // Estado WiFi
    static bool s_wifi_should_reconnect = false;
    static bool s_scan_task_created = false;
    
    // Task auxiliar para resetar flag de scan
    static void scan_reset_task(void *)
    {
        vTaskDelay(pdMS_TO_TICKS(30000));
        s_scan_task_created = false;
        vTaskDelete(nullptr);
    }

    // ---------------------------------------------------------------------
    // Mantém método privado conforme seu header; não expomos fora da classe
    bool Gateway::uart_write_json(const std::string &json)
    {
        char header[16];
        int n = snprintf(header, sizeof(header), "%u\n", (unsigned)json.size());
        if (n <= 0 || n >= (int)sizeof(header))
        {
            ESP_LOGE(TAG, "TX[UART GW->BORDER] erro ao formatar header (n=%d)", n);
            return false;
        }

        // Escreve header
        int w1 = uart_write_bytes(kUartNum, header, n);
        if (w1 < 0 || w1 != n)
        {
            ESP_LOGE(TAG, "TX[UART GW->BORDER] erro ao escrever header (w1=%d, esperado=%d)", w1, n);
            return false;
        }

        // Escreve JSON
        int w2 = uart_write_bytes(kUartNum, json.c_str(), json.size());
        if (w2 < 0 || w2 != (int)json.size())
        {
            ESP_LOGE(TAG, "TX[UART GW->BORDER] erro ao escrever JSON (w2=%d, esperado=%zu)", w2, json.size());
            return false;
        }

        // Aguarda transmissão completar
        uart_wait_tx_done(kUartNum, pdMS_TO_TICKS(1000));
        
        return true;
    }

    // Auxiliar local - lê exatamente 'len' bytes com timeout total
    static bool uart_read_exact(size_t len, std::string &out)
    {
        out.resize(len);
        size_t got = 0;
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000); // Timeout total de 2s
        
        while (got < len && xTaskGetTickCount() < deadline)
        {
            int r = uart_read_bytes(kUartNum, reinterpret_cast<uint8_t *>(&out[got]),
                                    len - got, pdMS_TO_TICKS(100));
            if (r > 0)
                got += r;
            else
                vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        if (got < len)
        {
            ESP_LOGW(TAG, "uart_read_exact: timeout - esperado %zu bytes, recebido %zu", len, got);
            return false;
        }
        return true;
    }

    // Callback para eventos WiFi
    static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
    {
        if (event_base == WIFI_EVENT)
        {
            switch (event_id)
            {
                case WIFI_EVENT_STA_START:
                    ESP_LOGI(TAG, "WiFi STA iniciado, conectando...");
                    // Conecta primeiro, scan será feito depois se necessário
                    esp_wifi_connect();
                    break;
                    
                case WIFI_EVENT_STA_DISCONNECTED:
                {
                    wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
                    const char* reason_str = "";
                    switch (disconnected->reason)
                    {
                        case WIFI_REASON_NO_AP_FOUND:
                            reason_str = "NO_AP_FOUND - Rede não encontrada (verifique SSID)";
                            // Se não encontrou a rede, faz scan para diagnosticar (apenas uma vez, não toda vez)
                            // Limita criação de tasks de scan para evitar múltiplas tasks
                            if (!s_scan_task_created)
                            {
                                s_scan_task_created = true;
                                ESP_LOGI(TAG, "Criando task de scan WiFi para diagnóstico...");
                                xTaskCreatePinnedToCore(wifi_scan_task, "wifi_scan", 4096, nullptr, 3, nullptr, tskNO_AFFINITY);
                                // Reset flag após 30 segundos para permitir novo scan se necessário
                                xTaskCreatePinnedToCore(scan_reset_task, "scan_reset", 1024, nullptr, 1, nullptr, tskNO_AFFINITY);
                            }
                            break;
                        case WIFI_REASON_AUTH_EXPIRE:
                            reason_str = "AUTH_EXPIRE - Autenticação expirada";
                            break;
                        case WIFI_REASON_AUTH_FAIL:
                            reason_str = "AUTH_FAIL - Senha incorreta";
                            break;
                        case WIFI_REASON_ASSOC_FAIL:
                            reason_str = "ASSOC_FAIL - Falha na associação";
                            break;
                        case WIFI_REASON_HANDSHAKE_TIMEOUT:
                            reason_str = "HANDSHAKE_TIMEOUT - Timeout no handshake";
                            break;
                        case WIFI_REASON_BEACON_TIMEOUT:
                            reason_str = "BEACON_TIMEOUT - Timeout no beacon";
                            break;
                        case WIFI_REASON_CONNECTION_FAIL:
                            reason_str = "CONNECTION_FAIL - Falha na conexão";
                            break;
                        default:
                            // Para códigos desconhecidos, mostra o número
                            if (disconnected->reason < 200)
                            {
                                reason_str = "Código padrão WiFi";
                            }
                            else
                            {
                                reason_str = "Código customizado/desconhecido";
                            }
                            break;
                    }
                    ESP_LOGW(TAG, "═══════════════════════════════════════════════════════");
                    ESP_LOGW(TAG, "WiFi desconectado!");
                    ESP_LOGW(TAG, "   Reason: %d - %s", disconnected->reason, reason_str);
                    ESP_LOGW(TAG, "   SSID: %s", disconnected->ssid);
                    ESP_LOGW(TAG, "   BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                             disconnected->bssid[0], disconnected->bssid[1], disconnected->bssid[2],
                             disconnected->bssid[3], disconnected->bssid[4], disconnected->bssid[5]);
                    ESP_LOGW(TAG, "═══════════════════════════════════════════════════════");
                    LedManager::set_gateway_server_connected(false);
                    // Sinaliza para task de reconexão (não bloqueia o handler)
                    s_wifi_should_reconnect = true;
                    break;
                }
                
                case WIFI_EVENT_STA_CONNECTED:
                {
                    wifi_event_sta_connected_t* connected = (wifi_event_sta_connected_t*) event_data;
                    ESP_LOGI(TAG, "WiFi conectado ao AP: %s (canal %d)", connected->ssid, connected->channel);
                    break;
                }
                
                default:
                    break;
            }
        }
        else if (event_base == IP_EVENT)
        {
            if (event_id == IP_EVENT_STA_GOT_IP)
            {
                ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
                ESP_LOGI(TAG, "WiFi conectado! IP: " IPSTR ", Máscara: " IPSTR ", Gateway: " IPSTR,
                         IP2STR(&event->ip_info.ip),
                         IP2STR(&event->ip_info.netmask),
                         IP2STR(&event->ip_info.gw));
                LedManager::set_gateway_server_connected(true);
            }
        }
    }

    // Inicializa e conecta WiFi
    static void init_wifi()
    {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "INICIANDO CONFIGURAÇÃO WIFI");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        // esp_netif_init e esp_event_loop já foram chamados no main.cpp
        esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
        if (!sta_netif)
        {
            ESP_LOGE(TAG, "Falha ao criar netif WiFi");
            return;
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao inicializar WiFi: %s", esp_err_to_name(err));
            return;
        }

        // Registra handlers de eventos
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao registrar handler WIFI_EVENT: %s", esp_err_to_name(err));
            return;
        }
        
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao registrar handler IP_EVENT: %s", esp_err_to_name(err));
            return;
        }

        // Obtém credenciais do menuconfig
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "LENDO CONFIGURAÇÕES WIFI DO MENUCONFIG");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        wifi_config_t wifi_config = {};
        memset(&wifi_config, 0, sizeof(wifi_config));
        
        const char* ssid = nullptr;
        const char* password = nullptr;
        
#ifdef CONFIG_WETZEL_GATEWAY_WIFI_SSID
        ssid = CONFIG_WETZEL_GATEWAY_WIFI_SSID;
        ESP_LOGI(TAG, "CONFIG_WETZEL_GATEWAY_WIFI_SSID = '%s'", ssid);
        ESP_LOGI(TAG, "   Tamanho: %zu bytes", strlen(ssid));
#else
        ESP_LOGW(TAG, "CONFIG_WETZEL_GATEWAY_WIFI_SSID não definido, usando padrão");
        ssid = "WetzelGateway";
#endif

#ifdef CONFIG_WETZEL_GATEWAY_WIFI_PASSWORD
        password = CONFIG_WETZEL_GATEWAY_WIFI_PASSWORD;
        ESP_LOGI(TAG, "CONFIG_WETZEL_GATEWAY_WIFI_PASSWORD = %s", 
                 (password && strlen(password) > 0) ? "*** (configurada)" : "(vazia)");
        if (password && strlen(password) > 0)
        {
            ESP_LOGI(TAG, "   Tamanho: %zu bytes", strlen(password));
        }
#else
        ESP_LOGW(TAG, "CONFIG_WETZEL_GATEWAY_WIFI_PASSWORD não definido, rede aberta");
        password = "";
#endif
        
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        // Processa SSID - remove espaços extras e adiciona logs detalhados
        if (ssid && strlen(ssid) > 0)
        {
            // Remove espaços extras no início e fim
            const char* ssid_start = ssid;
            size_t ssid_len = strlen(ssid);
            
            // Remove espaços no início
            while (ssid_len > 0 && (ssid_start[0] == ' ' || ssid_start[0] == '\t'))
            {
                ssid_start++;
                ssid_len--;
            }
            
            // Remove espaços no fim
            while (ssid_len > 0 && (ssid_start[ssid_len-1] == ' ' || ssid_start[ssid_len-1] == '\t'))
            {
                ssid_len--;
            }
            
            if (ssid_len > 0 && ssid_len < sizeof(wifi_config.sta.ssid))
            {
                memcpy(wifi_config.sta.ssid, ssid_start, ssid_len);
                wifi_config.sta.ssid[ssid_len] = '\0';
                
                // Log detalhado para debug - mostra cada caractere do SSID
                ESP_LOGI(TAG, "SSID processado: '%s' (len=%zu)", (char*)wifi_config.sta.ssid, ssid_len);
                ESP_LOGI(TAG, "SSID bytes hex (primeiros 32):");
                for (size_t i = 0; i < ssid_len && i < 32; i++)
                {
                    ESP_LOGI(TAG, "  [%zu] = 0x%02X ('%c')", i, (unsigned char)wifi_config.sta.ssid[i], 
                             (wifi_config.sta.ssid[i] >= 32 && wifi_config.sta.ssid[i] < 127) ? 
                             wifi_config.sta.ssid[i] : '?');
                }
            }
            else
            {
                ESP_LOGE(TAG, "SSID inválido (tamanho após trim: %zu, máximo: %zu)", 
                         ssid_len, sizeof(wifi_config.sta.ssid) - 1);
                return;
            }
        }
        else
        {
            ESP_LOGE(TAG, "SSID vazio ou NULL!");
            return;
        }
        
        // Processa senha - não força apenas WPA2_PSK, permite WPA3 e mixed
        if (password && strlen(password) > 0)
        {
            size_t pwd_len = strlen(password);
            if (pwd_len < sizeof(wifi_config.sta.password))
            {
                memcpy(wifi_config.sta.password, password, pwd_len);
                wifi_config.sta.password[pwd_len] = '\0';
                // Não força apenas WPA2_PSK - threshold mínimo WPA2, mas aceita WPA3 e mixed
                // O ESP-IDF tentará negociar automaticamente o melhor método suportado
                wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            }
            else
            {
                ESP_LOGE(TAG, "Senha muito longa (tamanho: %zu, máximo: %zu)", 
                         pwd_len, sizeof(wifi_config.sta.password) - 1);
                wifi_config.sta.password[0] = '\0';
                wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
            }
        }
        else
        {
            // Senha vazia = rede aberta
            wifi_config.sta.password[0] = '\0';
            wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }

        // Configurações WiFi
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
        wifi_config.sta.scan_method = WIFI_FAST_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        
        // Configurações adicionais para melhor compatibilidade
        wifi_config.sta.threshold.rssi = -127; // Aceita qualquer sinal (pode ajustar se necessário)
        wifi_config.sta.bssid_set = false; // Não força BSSID específico

        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "CONFIGURAÇÃO WIFI QUE SERÁ USADA:");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "   SSID: '%s'", (char*)wifi_config.sta.ssid);
        ESP_LOGI(TAG, "   Tamanho do SSID: %zu bytes", strlen((char*)wifi_config.sta.ssid));
        ESP_LOGI(TAG, "   Senha: %s", 
                 (password && strlen(password) > 0) ? "*** (configurada)" : "(vazia - rede aberta)");
        if (password && strlen(password) > 0)
        {
            ESP_LOGI(TAG, "   Tamanho da senha: %zu bytes", strlen(password));
        }
        ESP_LOGI(TAG, "   Autenticação mínima: %s (aceita WPA2/WPA3/mixed)", 
                 wifi_config.sta.threshold.authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2_PSK");
        ESP_LOGI(TAG, "   RSSI threshold: %d dBm", wifi_config.sta.threshold.rssi);
        ESP_LOGI(TAG, "   BSSID fixo: %s", wifi_config.sta.bssid_set ? "SIM" : "NÃO");
        ESP_LOGI(TAG, "   Scan method: FAST_SCAN");
        ESP_LOGI(TAG, "   Sort method: BY_SIGNAL");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao definir modo WiFi: %s", esp_err_to_name(err));
            return;
        }

        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao configurar WiFi: %s", esp_err_to_name(err));
            return;
        }

        err = esp_wifi_start();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao iniciar WiFi: %s", esp_err_to_name(err));
            return;
        }

        ESP_LOGI(TAG, "WiFi iniciado com sucesso. Aguardando conexão...");
        
        // Pequeno delay para garantir que WiFi está totalmente inicializado
        // O evento WIFI_EVENT_STA_START será disparado e chamará esp_wifi_connect()
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // ---------------------------------------------------------------------
    void Gateway::init(const std::string &server_url)
    {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "INICIALIZANDO GATEWAY");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        // Liberar memória BT (não usamos)
        esp_err_t r = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        if (r == ESP_OK)
            ESP_LOGI(TAG, "Memoria BT liberada.");
        else
            ESP_LOGW(TAG, "BT ja liberado ou ativo.");

        // Configurar URL do servidor (usa menuconfig se vazio)
        if (!server_url.empty())
        {
            s_server_url = server_url;
        }
        else
        {
#ifdef CONFIG_WETZEL_GATEWAY_SERVER_URL
            s_server_url = CONFIG_WETZEL_GATEWAY_SERVER_URL;
#else
            s_server_url = "http://192.168.1.100:8080"; // Fallback se constante não existir
#endif
        }
        ESP_LOGI(TAG, "Server URL: %s", s_server_url.c_str());
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

        // Inicializa WiFi
        init_wifi();

        // UART para comunicação com o nó-borda
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "CONFIGURANDO UART");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "   UART_NUM: %d", kUartNum);
        ESP_LOGI(TAG, "   TX Pin: %d (GPIO%d) -> Node RX (GPIO15)", kTxPin, kTxPin);
        ESP_LOGI(TAG, "   RX Pin: %d (GPIO%d) <- Node TX (GPIO13)", kRxPin, kRxPin);
        ESP_LOGI(TAG, "   Baud Rate: %d", kBaud);
        ESP_LOGI(TAG, "   Buffer Size: %zu bytes", kBufSize);
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        uart_config_t cfg{};
        cfg.baud_rate = kBaud;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

        // IMPORTANTE: Verifica se UART já está instalada e remove se necessário
        uart_driver_delete(kUartNum);
        
        esp_err_t err = uart_param_config(kUartNum, &cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao configurar parâmetros UART: %s", esp_err_to_name(err));
            return;
        }
        
        // Configura pinos ANTES de instalar o driver
        err = uart_set_pin(kUartNum, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao configurar pinos UART: %s", esp_err_to_name(err));
            return;
        }
        
        err = uart_driver_install(kUartNum, kBufSize, kBufSize, 0, nullptr, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao instalar driver UART: %s", esp_err_to_name(err));
            return;
        }
        
        ESP_LOGI(TAG, "UART configurada com sucesso!");
        
        // Delay maior para garantir que UART está totalmente inicializada
        vTaskDelay(pdMS_TO_TICKS(500)); // Aumentado de 100ms para 500ms

        // RX pela UART (GW <- Nó)
        ESP_LOGI(TAG, "Criando task de escuta UART...");
        xTaskCreatePinnedToCore(uart_listen_task, "gw_uart_rx", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
        ESP_LOGI(TAG, "Task de escuta UART criada! Gateway pronto para receber conexões.");

        // Task para processar requisições HTTP
        xTaskCreatePinnedToCore(http_request_task, "gw_http", 8192, nullptr, 5, nullptr, tskNO_AFFINITY);

        // Task para logar status UART periodicamente
        xTaskCreatePinnedToCore(uart_status_task, "gw_uart_status", 2048, nullptr, 3, nullptr, tskNO_AFFINITY);

        // Task para reconexão WiFi (não bloqueia handlers)
        xTaskCreatePinnedToCore(wifi_reconnect_task, "wifi_reconnect", 2048, nullptr, 3, nullptr, tskNO_AFFINITY);

        // Gera tráfego de teste HELLO → valida caminho completo UART→MESH
        xTaskCreatePinnedToCore(test_packet_task, "gw_uart_tx_test", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);

        ESP_LOGI(TAG, "Gateway inicializado (UART TX=%d RX=%d, Server=%s).", kTxPin, kRxPin, s_server_url.c_str());
    }
    
    void Gateway::set_server_url(const std::string &url)
    {
        s_server_url = url;
        ESP_LOGI(TAG, "Server URL atualizado: %s", s_server_url.c_str());
    }
    
    std::string Gateway::get_server_url()
    {
        return s_server_url;
    }

    void Gateway::uart_listen_task(void *)
    {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "UART LISTEN TASK INICIADA");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "Gateway aguardando dados do border node na UART...");
        ESP_LOGI(TAG, "UART_NUM: %d, RX Pin: GPIO%d", kUartNum, kRxPin);
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        std::string acc;
        acc.reserve(16);

        auto read_line = [&]() -> std::string
        {
            acc.clear();
            const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000); // Timeout de 5s para linha
            while (xTaskGetTickCount() < deadline)
            {
                uint8_t ch;
                int r = uart_read_bytes(kUartNum, &ch, 1, pdMS_TO_TICKS(50));
                if (r == 1)
                {
                    if (ch == '\n')
                        break;
                    if (acc.size() < 12)
                        acc.push_back((char)ch);
                }
                else
                    vTaskDelay(pdMS_TO_TICKS(5));
            }
            return acc;
        };

        // Log periódico para indicar que está escutando
        TickType_t last_status_log = xTaskGetTickCount();
        const TickType_t status_log_interval = pdMS_TO_TICKS(10000); // A cada 10 segundos
        
        for (;;)
        {
            // Log periódico mostrando que está escutando
            TickType_t now = xTaskGetTickCount();
            if (now - last_status_log >= status_log_interval)
            {
                ESP_LOGI(TAG, "UART: Gateway ainda escutando... (aguardando border node)");
                last_status_log = now;
            }
            
            // Verifica se há dados disponíveis antes de tentar ler
            size_t buffered_size = 0;
            uart_get_buffered_data_len(kUartNum, &buffered_size);
            if (buffered_size > 0)
            {
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                ESP_LOGI(TAG, "DADOS DETECTADOS NA UART: %zu bytes", buffered_size);
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
            }
            
            std::string lenStr = read_line();
            if (lenStr.empty())
            {
                // Timeout ou linha vazia - não é erro, apenas continua
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
            ESP_LOGI(TAG, "RX[UART GW] Header recebido: '%s'", lenStr.c_str());
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

            int len = atoi(lenStr.c_str());
            if (len <= 0 || len > (int)kBufSize)
            {
                ESP_LOGW(TAG, "RX[UART GW<-BORDER] tamanho inválido: '%s' (len=%d)", lenStr.c_str(), len);
                continue;
            }

            // ✅ CORREÇÃO: Marca UART como conectada assim que recebe header válido
            // Isso garante que a flag seja setada o mais cedo possível
            if (!LedManager::get_gateway_uart_connected())
            {
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                ESP_LOGI(TAG, "UART CONECTADA! Header válido recebido do border node");
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                LedManager::set_gateway_uart_connected(true);
            }

            std::string json;
            if (!uart_read_exact((size_t)len, json))
            {
                ESP_LOGW(TAG, "RX[UART GW<-BORDER] falha ao ler %d bytes", len);
                continue;
            }

            Protocol::Packet pkt;
            if (!Protocol::parse(json, pkt))
            {
                ESP_LOGW(TAG, "RX[UART GW<-BORDER] falha ao parsear JSON (%d bytes)", len);
                continue;
            }

            ESP_LOGI(TAG, "RX[UART GW<-BORDER] from=%s dst=%s (%d bytes)",
                     pkt.route.src.c_str(), pkt.route.dst.c_str(), len);
            // ✅ REMOVIDO: Flag já foi setada acima quando recebeu header válido
            // LedManager::set_gateway_uart_connected(true); // Marca UART como conectada
            LedManager::blink(TrafficSource::UART);

            if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "PING")
            {
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                ESP_LOGI(TAG, "PING recebido do border node! Enviando PONG...");
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                
                Protocol::Packet pong{};
                pong.type = Protocol::PacketType::EVENT;
                pong.method = "PONG";
                pong.route.src = "gateway";
                pong.route.dst = pkt.route.src;
                pong.body = R"({"status":"alive"})";

                std::string pong_json = Protocol::serialize(pong);
                ESP_LOGI(TAG, "TX[UART GW->BORDER] PONG para %s (%u bytes)", 
                         pong.route.dst.c_str(), (unsigned)pong_json.size());
                
                if (uart_write_json(pong_json))
                {
                    ESP_LOGI(TAG, "PONG enviado com sucesso!");
                }
                else
                {
                    ESP_LOGE(TAG, "Falha ao enviar PONG!");
                }
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                continue;
            }

            // Processa TOKEN (modo teste) - token voltou do border node
#ifdef CONFIG_WETZEL_TEST_MODE
            if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "TOKEN" && pkt.route.dst == "gateway")
            {
                ESP_LOGI(TAG, "TOKEN voltou ao Gateway de %s", pkt.route.src.c_str());
                s_gateway_has_token = true;
                // LED acende quando recebe token
                LedManager::set_led_on_for_duration(CONFIG_WETZEL_TOKEN_HOLD_TIME_MS);
                continue;
            }
#endif

            if (pkt.route.dst == "server" || pkt.type == Protocol::PacketType::REQUEST)
            {
                // Requisição HTTP - processa via cliente HTTP
                ESP_LOGI(TAG, "Requisição HTTP recebida: %s %s", pkt.method.c_str(), pkt.endpoint.c_str());
                send_http_request(pkt);
            }
        }
    }

    static void wifi_scan_task(void *)
    {
        // Aguarda mais tempo para garantir que WiFi não está mais tentando conectar
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "INICIANDO SCAN WIFI PARA DIAGNÓSTICO");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "Aguardando WiFi estabilizar antes do scan...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        // Verifica se WiFi já está conectado
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        {
            ESP_LOGI(TAG, "WiFi já conectado, pulando scan");
            vTaskDelete(nullptr);
            return;
        }
        
        // Verifica estado do WiFi antes de fazer scan
        wifi_mode_t mode;
        esp_err_t mode_err = esp_wifi_get_mode(&mode);
        if (mode_err != ESP_OK)
        {
            ESP_LOGW(TAG, "Não foi possível verificar modo WiFi: %s", esp_err_to_name(mode_err));
            vTaskDelete(nullptr);
            return;
        }
        
        // Se WiFi está em modo STA e não conectado, pode fazer scan sem parar
        if (mode != WIFI_MODE_STA)
        {
            ESP_LOGW(TAG, "WiFi não está em modo STA, abortando scan");
            vTaskDelete(nullptr);
            return;
        }
        
        ESP_LOGI(TAG, "WiFi em modo STA, fazendo scan sem parar WiFi...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Aguarda um pouco para WiFi estar estável
        
        // Mostra qual SSID está sendo procurado
        const char* target_ssid = nullptr;
#ifdef CONFIG_WETZEL_GATEWAY_WIFI_SSID
        target_ssid = CONFIG_WETZEL_GATEWAY_WIFI_SSID;
#endif
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "INICIANDO SCAN WIFI");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "Procurando pela rede: '%s'", target_ssid ? target_ssid : "(NULL)");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        wifi_scan_config_t scan_config = {};
        scan_config.ssid = NULL; // Scan todas as redes
        scan_config.bssid = NULL;
        scan_config.channel = 0; // Todos os canais
        scan_config.show_hidden = false;
        scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        scan_config.scan_time.active.min = 100;
        scan_config.scan_time.active.max = 300;
        
        esp_err_t scan_err = esp_wifi_scan_start(&scan_config, true); // true = bloqueante
        if (scan_err == ESP_OK)
        {
            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);
            ESP_LOGI(TAG, "Scan completo: %d redes encontradas", ap_count);
            
            if (ap_count > 0)
            {
                wifi_ap_record_t ap_records[20];
                uint16_t ap_records_count = (ap_count > 20) ? 20 : ap_count;
                esp_wifi_scan_get_ap_records(&ap_records_count, ap_records);
                
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                ESP_LOGI(TAG, "REDES DISPONÍVEIS:");
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                bool found_target = false;
                for (int i = 0; i < ap_records_count; i++)
                {
                    bool is_target = (target_ssid && strcmp((char*)ap_records[i].ssid, target_ssid) == 0);
                    if (is_target) found_target = true;
                    
                    ESP_LOGI(TAG, "   [%d] SSID: '%s' %s, RSSI: %d dBm, Auth: %d", 
                             i+1, ap_records[i].ssid, 
                             is_target ? "(ESTA É A REDE CONFIGURADA!)" : "",
                             ap_records[i].rssi, ap_records[i].authmode);
                }
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                
                if (!found_target && target_ssid)
                {
                    ESP_LOGW(TAG, "ATENÇÃO: A rede '%s' NÃO foi encontrada no scan!", target_ssid);
                    ESP_LOGW(TAG, "   Verifique se:");
                    ESP_LOGW(TAG, "   1. O SSID está correto no menuconfig");
                    ESP_LOGW(TAG, "   2. A rede está no alcance");
                    ESP_LOGW(TAG, "   3. A rede não está oculta (hidden)");
                }
            }
            else
            {
                ESP_LOGW(TAG, "Nenhuma rede encontrada no scan!");
            }
        }
        else
        {
            ESP_LOGW(TAG, "Falha no scan: %s", esp_err_to_name(scan_err));
        }
        
        // Não reinicia WiFi - deixa a task de reconexão cuidar disso
        ESP_LOGI(TAG, "Scan completo. WiFi continuará tentando conectar automaticamente.");
        
        vTaskDelete(nullptr); // Task única, se deleta após completar
    }

    static void wifi_reconnect_task(void *)
    {
        for (;;)
        {
            if (s_wifi_should_reconnect)
            {
                s_wifi_should_reconnect = false;
                vTaskDelay(pdMS_TO_TICKS(2000)); // Aguarda 2 segundos antes de reconectar
                ESP_LOGI(TAG, "Tentando reconectar WiFi...");
                esp_wifi_connect();
            }
            vTaskDelay(pdMS_TO_TICKS(500)); // Verifica a cada 500ms
        }
    }

    static void uart_status_task(void *)
    {
        bool last_uart_status = false;
        for (;;)
        {
            bool current_uart_status = LedManager::get_gateway_uart_connected();
            if (current_uart_status != last_uart_status)
            {
                if (current_uart_status)
                {
                    ESP_LOGI(TAG, "UART: CONECTADO com border node");
                }
                else
                {
                    ESP_LOGW(TAG, "UART: DESCONECTADO - aguardando border node...");
                }
                last_uart_status = current_uart_status;
            }
            vTaskDelay(pdMS_TO_TICKS(5000)); // Verifica a cada 5 segundos
        }
    }

    static void test_packet_task(void *)
    {
#ifdef CONFIG_WETZEL_TEST_MODE
        // Modo TESTE: Gateway inicia token passing
        ESP_LOGI(TAG, "Modo TESTE: Gateway iniciando token passing (não aguarda UART conectar)");
        vTaskDelay(pdMS_TO_TICKS(2000)); // Pequeno delay para sistema estabilizar
        
        // Inicia token imediatamente, sem esperar UART conectar
        ESP_LOGI(TAG, "Gateway iniciando token passing");
        s_gateway_has_token = true;
        
        for (;;)
        {
            // Se tem token, mantém por tempo configurado e depois passa
            if (s_gateway_has_token)
            {
                uint32_t hold_time = CONFIG_WETZEL_TOKEN_HOLD_TIME_MS;
                ESP_LOGI(TAG, "Gateway tem TOKEN - mantendo por %u ms", hold_time);
                vTaskDelay(pdMS_TO_TICKS(hold_time));
                
                // Passa token para border node via UART
                Protocol::Packet token{};
                token.type = Protocol::PacketType::EVENT;
                token.method = "TOKEN";
                token.route.src = "gateway";
                token.route.dst = "border"; // Border node recebe e passa adiante
                token.body = R"({"type":"token","from":"gateway"})";
                
                ESP_LOGI(TAG, "Gateway passando TOKEN para border node via UART");
                if (Gateway::send(token))
                {
                    s_gateway_has_token = false;
                    ESP_LOGI(TAG, "Token enviado via UART");
                }
                else
                {
                    ESP_LOGW(TAG, "Falha ao enviar token via UART - UART não conectada");
                }
            }
            else
            {
                // Se não tem token e UART não está conectada, não faz nada
                // Se não tem token mas UART está conectada, aguarda token voltar
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
#else
        // Modo REAL: Gateway gera e envia dados a cada 5 segundos infinitamente
        ESP_LOGI(TAG, "Modo REAL: Gateway iniciando geração de dados a cada 5 segundos");
        std::srand(static_cast<unsigned>(time(nullptr)));
        int seq = 0;
        
        // Pequeno delay inicial para sistema estabilizar
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        for (;;)
        {
            Protocol::Packet pkt{};
            pkt.type = Protocol::PacketType::EVENT;
            pkt.method = "DATA";
            pkt.route.src = "gateway";
            pkt.route.dst = "border";
            pkt.body = "{\"temp\":" + std::to_string(20 + (std::rand() % 10)) +
                       ",\"hum\":" + std::to_string(50 + (std::rand() % 20)) +
                       ",\"seq\":" + std::to_string(seq++) + "}";

            // Preencher informações de rastreabilidade
            uint64_t now_ms = esp_timer_get_time() / 1000ULL;
            pkt.trace.packet_id = Protocol::generate_packet_id();
            pkt.trace.created_at_ms = now_ms;
            pkt.trace.path.push_back("gateway"); // Inicia o caminho
            pkt.trace.hop_count = 0;
            pkt.routing_strategy = "flooding";
            pkt.ttl = 10;

            // Adicionar hop inicial do gateway
            Protocol::HopInfo gateway_hop;
            gateway_hop.node_id = "gateway";
            gateway_hop.node_name = "Gateway Principal";
            gateway_hop.timestamp_ms = now_ms;
            gateway_hop.transport = "UART";
            pkt.trace.hop_history.push_back(gateway_hop);

            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
            ESP_LOGI(TAG, "Gateway gerando e enviando dado #%d: %s", seq, pkt.body.c_str());
            ESP_LOGI(TAG, "Packet ID: %s", pkt.trace.packet_id.c_str());
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

            // ⚠️ Usa API pública, mantendo uart_write_json privada:
            bool sent = Gateway::send(pkt);
            if (sent)
            {
                ESP_LOGI(TAG, "Dado enviado com sucesso via UART");
            }
            else
            {
                ESP_LOGW(TAG, "Falha ao enviar dado via UART (continuando mesmo assim)");
            }

            ESP_LOGI(TAG, "Aguardando 5 segundos antes do próximo envio...");
            vTaskDelay(pdMS_TO_TICKS(5000)); // a cada 5 s
            ESP_LOGI(TAG, "5 segundos passaram, gerando próximo dado...");
        }
#endif
    }

    // ---------------------------------------------------------------------
    // Expostas para NetworkManager / Router (resolvem os 'undefined reference')
    bool Gateway::send(const Protocol::Packet &pkt)
    {
        std::string json = Protocol::serialize(pkt);
        ESP_LOGI(TAG, "TX[UART GW->BORDER] via Gateway::send (%u bytes)", (unsigned)json.size());
        LedManager::blink(TrafficSource::UART);
        return uart_write_json(json);
    }

    bool Gateway::send_to_border(const Protocol::Packet &pkt)
    {
        return send(pkt);
    }
    
    // Estrutura para passar dados para o callback HTTP
    struct HttpCallbackData
    {
        std::string request_id;
        std::string response_body;
    };
    
    // Callback do cliente HTTP para receber resposta
    static esp_err_t http_event_handler(esp_http_client_event_t *evt)
    {
        HttpCallbackData *data = static_cast<HttpCallbackData *>(evt->user_data);
        if (!data)
            return ESP_FAIL;
        
        switch (evt->event_id)
        {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            data->response_body.clear();
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client))
            {
                data->response_body.append((char *)evt->data, evt->data_len);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            {
                // Recupera contexto
                auto ctx = HttpContextManager::instance().get_context(data->request_id);
                if (ctx)
                {
                    int status_code = esp_http_client_get_status_code(evt->client);
                    
                    // Cria resposta
                    Protocol::Packet response;
                    response.type = Protocol::PacketType::RESPONSE;
                    response.route.src = "gateway";
                    response.route.dst = ctx->original_src;
                    response.status = status_code;
                    response.body = data->response_body;
                    response.request_id = data->request_id;
                    
                    ESP_LOGI(TAG, "Resposta HTTP: status=%d body_size=%u", status_code, (unsigned)data->response_body.size());
                    
                    // Se resposta é muito grande, particiona em chunks
                    if (data->response_body.size() > kMaxChunkSize)
                    {
                        auto chunks = Protocol::create_chunks(response, kMaxChunkSize);
                        ESP_LOGI(TAG, "Resposta grande, dividindo em %u chunks", (unsigned)chunks.size());
                        
                        for (const auto &chunk : chunks)
                        {
                            Gateway::send_to_border(chunk);
                            vTaskDelay(pdMS_TO_TICKS(10)); // Pequeno delay entre chunks
                        }
                    }
                    else
                    {
                        Gateway::send_to_border(response);
                    }
                    
                    // Remove contexto
                    HttpContextManager::instance().remove_context(data->request_id);
                }
            }
            data->response_body.clear();
            // Limpa callback_data após processar resposta
            delete data;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            // Em caso de desconexão antes de FINISH, limpa dados
            if (data)
            {
                delete data;
            }
            break;
        default:
            break;
        }
        return ESP_OK;
    }
    
    bool Gateway::send_http_request(const Protocol::Packet &request_pkt)
    {
        // Cria contexto
        std::string request_id = HttpContextManager::instance().create_context(request_pkt, s_server_url);
        
        // Constrói URL completa
        std::string full_url = s_server_url + request_pkt.endpoint;
        
        ESP_LOGI(TAG, "Enviando requisição HTTP: %s %s", request_pkt.method.c_str(), full_url.c_str());
        
        // Cria estrutura de dados para callback (alocada dinamicamente)
        // Nota: O callback HTTP pode ser assíncrono, então mantemos o ponteiro
        // até que o callback seja chamado. O cleanup será feito no callback.
        HttpCallbackData *callback_data = new HttpCallbackData();
        callback_data->request_id = request_id;
        callback_data->response_body.clear();
        
        esp_http_client_config_t config = {};
        config.url = full_url.c_str();
        config.event_handler = http_event_handler;
        config.user_data = callback_data;
        config.timeout_ms = 30000; // 30 segundos
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client)
        {
            ESP_LOGE(TAG, "Falha ao inicializar cliente HTTP");
            delete callback_data;
            HttpContextManager::instance().remove_context(request_id);
            return false;
        }
        
        // Configura método HTTP
        esp_http_client_set_method(client, 
            (request_pkt.method == "POST") ? HTTP_METHOD_POST :
            (request_pkt.method == "PUT") ? HTTP_METHOD_PUT :
            (request_pkt.method == "DELETE") ? HTTP_METHOD_DELETE :
            HTTP_METHOD_GET);
        
        // Adiciona headers
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "X-Request-ID", request_id.c_str());
        esp_http_client_set_header(client, "X-Original-Src", request_pkt.route.src.c_str());
        
        // Se tem body, adiciona
        if (!request_pkt.body.empty())
        {
            esp_http_client_set_post_field(client, request_pkt.body.c_str(), request_pkt.body.length());
        }
        
        // Executa requisição
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "HTTP Status = %d, content_length = %lld",
                     esp_http_client_get_status_code(client),
                     esp_http_client_get_content_length(client));
        }
        else
        {
            ESP_LOGE(TAG, "Erro ao executar requisição HTTP: %s", esp_err_to_name(err));
            
            // Envia resposta de erro
            Protocol::Packet error_response;
            error_response.type = Protocol::PacketType::RESPONSE;
            error_response.route.src = "gateway";
            error_response.route.dst = request_pkt.route.src;
            error_response.status = 500;
            error_response.body = R"({"error":"HTTP request failed"})";
            error_response.request_id = request_id;
            send_to_border(error_response);
            
            HttpContextManager::instance().remove_context(request_id);
        }
        
        esp_http_client_cleanup(client);
        
        // Nota: callback_data será deletado no callback HTTP_EVENT_ON_FINISH
        // Se houver erro antes do callback, deleta aqui
        if (err != ESP_OK && callback_data)
        {
            delete callback_data;
        }
        
        LedManager::blink(TrafficSource::SERVER);
        return (err == ESP_OK);
    }
    
    void Gateway::http_request_task(void *)
    {
        // Task para limpar contextos antigos periodicamente
        for (;;)
        {
            vTaskDelay(pdMS_TO_TICKS(60000)); // A cada 1 minuto
            HttpContextManager::instance().cleanup_old_contexts(300000); // 5 minutos
        }
    }

} // namespace WetzelMesh
