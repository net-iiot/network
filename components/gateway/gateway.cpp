#include "gateway.hpp"
#include "http_context.hpp"
#include "esp_log.h"
#include "driver/uart.h"
#include "protocol.hpp"
#include "network_manager.hpp"
#include "network_mapper.hpp"
#include "ota_manager.hpp"
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
#include <sstream>

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

    static void uart_status_task(void *);
    static void wifi_reconnect_task(void *);
    static void wifi_scan_task(void *);
    
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

        // Task de polling unificada: map + OTA commands
        xTaskCreatePinnedToCore(polling_task, "gw_polling", 8192, nullptr, 4, nullptr, tskNO_AFFINITY);

        // Registra callback no OTAManager para que ele possa enviar para border
        OTAManager::set_send_to_border_callback([](const Protocol::Packet &pkt) {
            return Gateway::send_to_border(pkt);
        });

        // Registra callback de report OTA: gateway envia HTTP diretamente ao servidor
        OTAManager::set_report_callback([](bool success, const std::string &command_id, const std::string &node_id, const std::string &error_msg) {
            Gateway::send_ota_report(success, command_id, node_id, error_msg);
        });

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

            // Processa resposta DISCOVERY_RESPONSE (mapeamento)
            if (pkt.type == Protocol::PacketType::RESPONSE && pkt.method == "DISCOVERY_RESPONSE")
            {
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                ESP_LOGI(TAG, "GATEWAY: Resposta DISCOVERY recebida via UART");
                ESP_LOGI(TAG, "   Origem: %s", pkt.route.src.c_str());
                ESP_LOGI(TAG, "   Destino: %s", pkt.route.dst.c_str());
                ESP_LOGI(TAG, "   Node ID (topology): %s", pkt.topology.node_id.c_str());
                ESP_LOGI(TAG, "   Node ID (node_info): %s", pkt.topology.node_info.node_id.c_str());
                ESP_LOGI(TAG, "   Node Type: %s", pkt.topology.node_info.node_type.c_str());
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                NetworkMapper::on_discovery_response(pkt);
                continue;
            }

            // ✅ NOVO: Processa eventos NODE_JOINED, NODE_LEFT e OTA_RESULT
            if (pkt.type == Protocol::PacketType::EVENT)
            {
                if (pkt.method == "NODE_JOINED")
                {
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    ESP_LOGI(TAG, "GATEWAY: Evento NODE_JOINED recebido via UART");
                    ESP_LOGI(TAG, "   Body: %s", pkt.body.c_str());
                    ESP_LOGI(TAG, "   Disparando mapeamento imediato...");
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    NetworkMapper::trigger_mapping();
                    continue;
                }
                else if (pkt.method == "NODE_LEFT")
                {
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    ESP_LOGI(TAG, "GATEWAY: Evento NODE_LEFT recebido via UART");
                    ESP_LOGI(TAG, "   Body: %s", pkt.body.c_str());
                    ESP_LOGI(TAG, "   Disparando mapeamento imediato...");
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    NetworkMapper::trigger_mapping();
                    continue;
                }
                else if (pkt.method == "OTA_RESULT")
                {
                    // Node enviou resultado de OTA - repassa ao servidor via HTTP
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    ESP_LOGI(TAG, "GATEWAY: Resultado OTA recebido de node via UART");
                    ESP_LOGI(TAG, "   Src: %s", pkt.route.src.c_str());
                    ESP_LOGI(TAG, "   Body: %s", pkt.body.c_str());
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

                    // Extrai campos do body JSON
                    std::string command_id, node_id, error_msg;
                    bool success = false;

                    auto extract = [&](const std::string &key, bool is_bool = false) -> std::string {
                        std::string search = "\"" + key + "\":";
                        size_t pos = pkt.body.find(search);
                        if (pos == std::string::npos) return "";
                        pos += search.size();
                        if (is_bool) {
                            return (pkt.body.substr(pos, 4) == "true") ? "true" : "false";
                        }
                        if (pkt.body[pos] == '"') {
                            pos++;
                            size_t end = pkt.body.find('"', pos);
                            return (end != std::string::npos) ? pkt.body.substr(pos, end - pos) : "";
                        }
                        return "";
                    };

                    command_id = extract("command_id");
                    node_id = extract("node_id");
                    if (node_id.empty()) node_id = pkt.route.src;
                    error_msg = extract("error");
                    success = (extract("success", true) == "true");

                    if (!command_id.empty())
                    {
                        send_ota_report(success, command_id, node_id, error_msg);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "OTA_RESULT sem command_id, ignorando");
                    }
                    continue;
                }
            }

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
        bool deleted = false; // Flag para evitar double-free
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
            {
                // Verifica se é erro de conexão
                if (data && !data->deleted)
                {
                    // Tenta recuperar contexto antes de enviar resposta
                    auto ctx = HttpContextManager::instance().get_context(data->request_id);
                    if (ctx)
                    {
                        // Envia resposta de erro apenas se contexto existe
                        Protocol::Packet error_response;
                        error_response.type = Protocol::PacketType::RESPONSE;
                        error_response.route.src = "gateway";
                        error_response.route.dst = ctx->original_src;
                        error_response.request_id = data->request_id;
                        error_response.status = 500;
                        error_response.body = R"({"error":"HTTP connection failed"})";
                        
                        ESP_LOGW(TAG, "Enviando resposta de erro para: %s", ctx->original_src.c_str());
                        Gateway::send_to_border(error_response);
                        HttpContextManager::instance().remove_context(data->request_id);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Contexto não encontrado para request_id: %s", data->request_id.c_str());
                    }
                    
                    // Marca como deletado antes de deletar
                    data->deleted = true;
                    delete data;
                    data = nullptr;
                }
            }
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
                if (data && !data->deleted)
                {
                    // Recupera contexto
                    auto ctx = HttpContextManager::instance().get_context(data->request_id);
                    if (ctx)
                    {
                        int status_code = esp_http_client_get_status_code(evt->client);
                        
                        // Pisca LED quando requisição HTTP é bem-sucedida
                        if (status_code >= 200 && status_code < 300)
                        {
                            LedManager::blink(TrafficSource::SERVER);
                        }
                        
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
                    
                    // Marca como deletado antes de deletar
                    data->deleted = true;
                    delete data;
                    data = nullptr;
                }
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            // Em caso de desconexão antes de FINISH, limpa dados
            // Mas só se ainda não foi deletado no ON_FINISH ou ERROR
            if (data && !data->deleted)
            {
                // Verifica se o contexto ainda existe (se não existe, já foi processado)
                auto ctx = HttpContextManager::instance().get_context(data->request_id);
                if (!ctx)
                {
                    // Contexto já foi removido, então ON_FINISH já foi chamado e deletou data
                    // Não deleta novamente
                    data = nullptr;
                }
                else
                {
                    // Contexto ainda existe, então ON_FINISH não foi chamado
                    // Remove contexto e deleta data
                    HttpContextManager::instance().remove_context(data->request_id);
                    data->deleted = true;
                    delete data;
                    data = nullptr;
                }
            }
            break;
        default:
            break;
        }
        return ESP_OK;
    }
    
    bool Gateway::send_http_request(const Protocol::Packet &request_pkt)
    {
        // Verificar se WiFi está conectado antes de tentar HTTP
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK)
        {
            ESP_LOGW(TAG, "WiFi não conectado, não é possível enviar requisição HTTP");
            
            // Envia resposta de erro
            Protocol::Packet error_response;
            error_response.type = Protocol::PacketType::RESPONSE;
            error_response.route.src = "gateway";
            error_response.route.dst = request_pkt.route.src;
            error_response.status = 503; // Service Unavailable
            error_response.body = R"({"error":"WiFi not connected"})";
            error_response.request_id = HttpContextManager::instance().create_context(request_pkt, s_server_url);
            send_to_border(error_response);
            return false;
        }
        
        // Cria contexto
        std::string request_id = HttpContextManager::instance().create_context(request_pkt, s_server_url);

        // Gateway burro: SEMPRE envia para /api/packet
        // O network_service no servidor interpreta e roteia
        std::string full_url = s_server_url + "/api/packet";

        // Serializa o pacote COMPLETO como JSON para o body
        std::string packet_json = Protocol::serialize(request_pkt);

        ESP_LOGI(TAG, "Enviando pacote para network_service: method=%s src=%s dst=%s endpoint=%s",
                 request_pkt.method.c_str(), request_pkt.route.src.c_str(),
                 request_pkt.route.dst.c_str(), request_pkt.endpoint.c_str());
        
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

        // Gateway burro: sempre POST para enviar pacote completo
        esp_http_client_set_method(client, HTTP_METHOD_POST);

        // Adiciona headers
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "X-Request-ID", request_id.c_str());
        esp_http_client_set_header(client, "X-Original-Src", request_pkt.route.src.c_str());

        // Envia o pacote COMPLETO serializado como body
        esp_http_client_set_post_field(client, packet_json.c_str(), packet_json.length());
        
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
            
            // Se houver erro, o callback HTTP_EVENT_ERROR pode ser chamado e deletar o callback_data
            // Mas se não for chamado, precisamos deletar aqui
            // Verifica se já foi deletado pelo callback usando a flag
            if (callback_data && !callback_data->deleted)
            {
                callback_data->deleted = true;
                delete callback_data;
                callback_data = nullptr;
            }
        }
        
        esp_http_client_cleanup(client);
        
        // callback_data será deletado no callback HTTP_EVENT_ON_FINISH ou HTTP_EVENT_DISCONNECTED
        // Não deleta aqui para evitar double-free
        
        // LED já pisca no HTTP_EVENT_ON_FINISH quando status é 2xx
        // Não precisa piscar aqui também para evitar duplicação
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

    static void check_ota_commands();

    void Gateway::polling_task(void *)
    {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "TASK DE POLLING UNIFICADA INICIADA");
        ESP_LOGI(TAG, "   - Envio periódico de map para servidor");
        ESP_LOGI(TAG, "   - Verificação de comandos OTA do servidor");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        // Aguarda alguns segundos para rede estabilizar
        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 segundos
        
        uint64_t last_map_send_ms = 0;
        uint64_t last_ota_check_ms = 0;
        const uint32_t map_send_interval_ms = 300000;  // 5 minutos
        const uint32_t ota_check_interval_ms = 30000;  // 30 segundos
        
        for (;;)
        {
            uint64_t now_ms = esp_timer_get_time() / 1000ULL;
            
            // 1. Envia map periodicamente (se houver map disponível)
            if ((now_ms - last_map_send_ms) >= map_send_interval_ms)
            {
                NetworkMap last_map = NetworkMapper::get_last_map();
                if (last_map.nodes.size() > 0 || last_map.connectivity.connections.size() > 0)
                {
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    ESP_LOGI(TAG, "POLLING: Enviando map para servidor");
                    ESP_LOGI(TAG, "   Nodes: %u", (unsigned)last_map.nodes.size());
                    ESP_LOGI(TAG, "   Conexões: %u", (unsigned)last_map.connectivity.connections.size());
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    
                    // Constrói JSON com o mapeamento completo
                    std::string json = "{";
                    json += "\"mapped_at_ms\":" + std::to_string(last_map.mapped_at_ms) + ",";
                    json += "\"nodes\":[";
                    
                    bool first = true;
                    for (const auto &node : last_map.nodes)
                    {
                        if (!first) json += ",";
                        first = false;
                        
                        json += "{";
                        json += "\"node_id\":\"" + node.node_id + "\",";
                        if (!node.node_name.empty())
                            json += "\"node_name\":\"" + node.node_name + "\",";
                        json += "\"node_type\":\"" + (node.node_type.empty() ? "normal" : node.node_type) + "\",";
                        json += "\"rssi\":" + std::to_string(node.rssi) + ",";
                        json += "\"discovered_at_ms\":" + std::to_string(node.discovered_at_ms) + ",";
                        json += "\"position_x\":" + std::to_string(node.position_x) + ",";
                        json += "\"position_y\":" + std::to_string(node.position_y) + ",";
                        json += "\"neighbors_count\":" + std::to_string(node.neighbors.size());
                        json += "}";
                    }
                    
                    json += "],";
                    json += "\"connectivity\":{\"connections\":[";

                    first = true;
                    for (const auto &conn : last_map.connectivity.connections)
                    {
                        if (!first) json += ",";
                        first = false;

                        json += "{";
                        json += "\"from_node_id\":\"" + conn.from_node_id + "\",";
                        json += "\"to_node_id\":\"" + conn.to_node_id + "\",";
                        json += "\"rssi\":" + std::to_string(conn.rssi) + ",";
                        json += "\"is_direct\":" + std::string(conn.is_direct ? "true" : "false") + ",";
                        json += "\"last_communication_ms\":" + std::to_string(conn.last_communication_ms) + ",";
                        json += "\"packet_count\":" + std::to_string(conn.packet_count);
                        json += "}";
                    }

                    json += "]}";
                    json += "}";
                    
                    // Envia para o servidor via HTTP
                    Protocol::Packet map_packet{};
                    map_packet.type = Protocol::PacketType::REQUEST;
                    map_packet.method = "POST";
                    map_packet.route.src = "gateway";
                    map_packet.route.dst = "server";
                    map_packet.endpoint = "/api/network/map";
                    map_packet.body = json;
                    
                    send_http_request(map_packet);
                    last_map_send_ms = now_ms;
                }
                else
                {
                    ESP_LOGD(TAG, "POLLING: Map vazio, pulando envio");
                }
            }
            
            // 2. Verifica comandos OTA do servidor
            if ((now_ms - last_ota_check_ms) >= ota_check_interval_ms)
            {
                ESP_LOGI(TAG, "POLLING: Verificando comandos OTA do servidor...");
                check_ota_commands();
                last_ota_check_ms = now_ms;
            }
            
            // Aguarda 1 segundo antes da próxima iteração
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    
    // Envia feedback de resultado OTA ao servidor via HTTP POST
    void Gateway::send_ota_report(bool success, const std::string &command_id, const std::string &node_id, const std::string &error_msg)
    {
        std::string report_url = s_server_url + "/api/firmware/ota/report";

        // Monta body JSON
        std::ostringstream body;
        body << R"({"command_id":")" << command_id
             << R"(","node_id":")" << (node_id.empty() ? "gateway" : node_id)
             << R"(","success":)" << (success ? "true" : "false");
        if (!error_msg.empty())
            body << R"(,"error":")" << error_msg << "\"";
        body << "}";

        std::string body_str = body.str();

        esp_http_client_config_t config = {};
        config.url = report_url.c_str();
        config.timeout_ms = 5000;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client)
        {
            ESP_LOGW(TAG, "OTA report: falha ao criar cliente HTTP");
            return;
        }

        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body_str.c_str(), (int)body_str.size());

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK)
        {
            int status_code = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "OTA report enviado: command_id=%s, node=%s, success=%s, status_http=%d",
                     command_id.c_str(), node_id.c_str(), success ? "true" : "false", status_code);
        }
        else
        {
            ESP_LOGW(TAG, "OTA report falhou: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    }

    static void check_ota_commands()
    {
        // Verifica se há comandos OTA pendentes no servidor
        std::string commands_url = Gateway::get_server_url() + "/api/firmware/ota/commands?gateway_id=gateway";

        std::string response_body;

        // Usa event handler para capturar o body da resposta
        auto ota_http_event = [](esp_http_client_event_t *evt) -> esp_err_t {
            if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data)
            {
                std::string *body = static_cast<std::string *>(evt->user_data);
                body->append((char *)evt->data, evt->data_len);
            }
            return ESP_OK;
        };

        esp_http_client_config_t config = {};
        config.url = commands_url.c_str();
        config.timeout_ms = 5000;
        config.event_handler = ota_http_event;
        config.user_data = &response_body;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client)
        {
            ESP_LOGW(TAG, "Falha ao criar cliente HTTP para verificar comandos OTA");
            return;
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK)
        {
            int status_code = esp_http_client_get_status_code(client);

            if (status_code == 200 && !response_body.empty())
            {
                ESP_LOGI(TAG, "Comandos OTA recebidos: %s", response_body.c_str());
                OTAManager::process_ota_commands(response_body);
            }
            else if (status_code == 200)
            {
                ESP_LOGD(TAG, "Nenhum comando OTA pendente");
            }
            else
            {
                ESP_LOGW(TAG, "Erro ao verificar comandos OTA (status: %d)", status_code);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Erro ao verificar comandos OTA: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    }

} // namespace WetzelMesh
