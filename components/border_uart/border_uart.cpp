#include "border_uart.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "protocol.hpp"
#include "led_manager.hpp"
#include "ble_transport.hpp"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "espnow_transport.hpp"
#include <vector>
#include <cstdlib>

namespace WetzelMesh
{
    static const char *TAG = "BORDER_UART";

    static constexpr uart_port_t kUartNum = UART_NUM_1;
    static constexpr int kTxPin = 15; // Teste: TX -> RX do Gateway (GPIO13)
    static constexpr int kRxPin = 13; // Teste: RX <- TX do Gateway (GPIO15)
    static constexpr int kBaud = 115200;
    static constexpr size_t kBufSize = 2048;

    static bool s_enabled = false;
    static BorderUart::RxHandler s_rx_handler = nullptr;

    static void handshake_retry_task(void *);

    bool BorderUart::init()
    {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "CONFIGURANDO UART (BORDER NODE)");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "   UART_NUM: %d", kUartNum);
        ESP_LOGI(TAG, "   TX Pin: %d (GPIO%d) -> Gateway RX (GPIO16)", kTxPin, kTxPin);
        ESP_LOGI(TAG, "   RX Pin: %d (GPIO%d) <- Gateway TX (GPIO17)", kRxPin, kRxPin);
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
            return false;
        }
        
        // Configura pinos ANTES de instalar o driver
        err = uart_set_pin(kUartNum, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao configurar pinos UART: %s", esp_err_to_name(err));
            return false;
        }
        
        err = uart_driver_install(kUartNum, kBufSize, kBufSize, 0, nullptr, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao instalar driver UART: %s", esp_err_to_name(err));
            return false;
        }

        ESP_LOGI(TAG, "UART configurada com sucesso!");
        
        // Delay maior para garantir que UART está totalmente inicializada
        vTaskDelay(pdMS_TO_TICKS(500)); // Aumentado de 100ms para 500ms
        
        LedManager::set_uart_enabled(false);
        ESP_LOGI(TAG, "Criando task de handshake...");
        xTaskCreatePinnedToCore(handshake_retry_task, "border_uart_hs", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
        ESP_LOGI(TAG, "Task de handshake criada! Aguardando Gateway estar pronto...");
        vTaskDelay(pdMS_TO_TICKS(2000)); // Aumentado de 500ms para 2000ms - dá mais tempo para Gateway inicializar
        return true;
    }

    bool BorderUart::is_enabled() { return s_enabled; }
    void BorderUart::set_rx_handler(RxHandler cb) { s_rx_handler = cb; }

    // ---------------------------------------------------------------------
    // Método público (declarado no .hpp) — framing <len>\n<json>
    bool BorderUart::uart_write_json(const std::string &json)
    {
        char header[16];
        int n = snprintf(header, sizeof(header), "%u\n", (unsigned)json.size());
        if (n <= 0 || n >= (int)sizeof(header))
        {
            ESP_LOGE(TAG, "TX[UART BORDER->GW] erro ao formatar header (n=%d)", n);
            return false;
        }
        
        ESP_LOGI(TAG, "TX[UART BORDER->GW] Enviando header: '%s' (len=%d)", header, n);
        
        // Escreve header
        int w1 = uart_write_bytes(kUartNum, header, n);
        ESP_LOGI(TAG, "TX[UART BORDER->GW] uart_write_bytes header retornou: %d (esperado: %d)", w1, n);
        if (w1 < 0 || w1 != n)
        {
            ESP_LOGE(TAG, "TX[UART BORDER->GW] erro ao escrever header (w1=%d, esperado=%d)", w1, n);
            return false;
        }
        
        // Escreve JSON
        int w2 = uart_write_bytes(kUartNum, json.c_str(), json.size());
        ESP_LOGI(TAG, "TX[UART BORDER->GW] uart_write_bytes JSON retornou: %d (esperado: %zu)", w2, json.size());
        if (w2 < 0 || w2 != (int)json.size())
        {
            ESP_LOGE(TAG, "TX[UART BORDER->GW] erro ao escrever JSON (w2=%d, esperado=%zu)", w2, json.size());
            return false;
        }
        
        // Aguarda transmissão completar
        esp_err_t wait_err = uart_wait_tx_done(kUartNum, pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "TX[UART BORDER->GW] uart_wait_tx_done retornou: %s", esp_err_to_name(wait_err));
        
        return true;
    }

    // ---------------------------------------------------------------------
    bool BorderUart::send_to_gateway(const Protocol::Packet &pkt)
    {
        if (!s_enabled)
            return false;
        std::string json = Protocol::serialize(pkt);
        ESP_LOGI(TAG, "TX[UART BORDER->GW] %s -> %s (%u bytes)",
                 pkt.route.src.c_str(), pkt.route.dst.c_str(), (unsigned)json.size());
        BorderUart::uart_write_json(json);
        LedManager::blink(TrafficSource::UART);
        return true;
    }

    // ---------------------------------------------------------------------
    bool BorderUart::do_handshake(unsigned timeout_ms)
    {
        constexpr int kMaxRetries = 3;
        constexpr unsigned kRetryDelayMs = 500;
        
        for (int retry = 0; retry < kMaxRetries; retry++)
        {
            if (retry > 0)
            {
                ESP_LOGW(TAG, "Handshake retry %d/%d", retry, kMaxRetries);
                vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            }
            
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
            ESP_LOGI(TAG, "Enviando PING para gateway (tentativa %d/%d)...", retry + 1, kMaxRetries);
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
            
            Protocol::Packet ping{};
            ping.type = Protocol::PacketType::EVENT;
            ping.method = "PING";
            ping.route.src = "border";
            ping.route.dst = "gateway";
            ping.body = "{}";

            std::string json = Protocol::serialize(ping);
            ESP_LOGI(TAG, "PING serializado: %u bytes", (unsigned)json.size());
            
            if (!BorderUart::uart_write_json(json))
            {
                ESP_LOGE(TAG, "Falha ao enviar PING!");
                continue;
            }
            
            ESP_LOGI(TAG, "PING enviado com sucesso! Aguardando PONG...");

            std::string acc;
            acc.reserve(16);

            auto read_line = [&]() -> std::string
            {
                acc.clear();
                const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
                while (xTaskGetTickCount() < deadline)
                {
                    uint8_t ch;
                    int r = uart_read_bytes(kUartNum, &ch, 1, pdMS_TO_TICKS(20));
                    if (r == 1)
                    {
                        if (ch == '\n')
                            break;
                        if (acc.size() < 12)
                            acc.push_back((char)ch);
                    }
                }
                return acc;
            };

            // Aguarda resposta PONG dentro do loop de retry
            ESP_LOGI(TAG, "Aguardando PONG (timeout: %u ms)...", timeout_ms);
            const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
            while (xTaskGetTickCount() < deadline)
            {
                // Verifica se há dados disponíveis
                size_t buffered_size = 0;
                uart_get_buffered_data_len(kUartNum, &buffered_size);
                if (buffered_size > 0)
                {
                    ESP_LOGI(TAG, "Dados disponíveis na UART: %zu bytes", buffered_size);
                }
                
                std::string lenStr = read_line();
                if (lenStr.empty())
                {
                    // Timeout na leitura da linha - continua tentando até deadline
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                
                ESP_LOGI(TAG, "Header recebido no handshake: '%s'", lenStr.c_str());
                    
                int len = atoi(lenStr.c_str());
                if (len <= 0 || len > (int)kBufSize)
                {
                    ESP_LOGW(TAG, "Tamanho inválido no handshake: '%s' (len=%d)", lenStr.c_str(), len);
                    continue;
                }

                ESP_LOGI(TAG, "Lendo %d bytes do PONG...", len);
                std::vector<uint8_t> jsonBuf(len);
                size_t got = 0;
                const TickType_t read_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
                while (got < (size_t)len && xTaskGetTickCount() < read_deadline)
                {
                    int r = uart_read_bytes(kUartNum, jsonBuf.data() + got, len - got, pdMS_TO_TICKS(20));
                    if (r > 0)
                        got += r;
                }
                
                if (got < (size_t)len)
                {
                    ESP_LOGW(TAG, "Timeout ao ler resposta do handshake (esperado %d, recebido %zu)", len, got);
                    break; // Sai do loop interno, tenta novamente
                }

                Protocol::Packet pkt;
                std::string body(reinterpret_cast<char *>(jsonBuf.data()), len);
                ESP_LOGI(TAG, "Dados recebidos: %zu bytes", body.size());
                
                if (!Protocol::parse(body, pkt))
                {
                    ESP_LOGW(TAG, "Falha ao parsear resposta do handshake");
                    ESP_LOGW(TAG, "Body recebido: %s", body.c_str());
                    continue;
                }

                ESP_LOGI(TAG, "Pacote parseado: type=%d method='%s' src='%s' dst='%s'", 
                         (int)pkt.type, pkt.method.c_str(), pkt.route.src.c_str(), pkt.route.dst.c_str());

                if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "PONG")
                {
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    ESP_LOGI(TAG, "Handshake OK! PONG recebido do gateway!");
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                    return true;
                }
                else
                {
                    ESP_LOGW(TAG, "Resposta inesperada no handshake: type=%d method='%s'", 
                             (int)pkt.type, pkt.method.c_str());
                }
            }
            
            // Se chegou aqui, timeout sem receber PONG nesta tentativa
            ESP_LOGW(TAG, "Handshake timeout (tentativa %d/%d)", retry + 1, kMaxRetries);
        }
        
        ESP_LOGE(TAG, "Handshake falhou após %d tentativas", kMaxRetries);
        return false;
    }

    // ---------------------------------------------------------------------
    void BorderUart::uart_listen_task(void *)
    {
        ESP_LOGI(TAG, "UART listen task iniciada - aguardando dados do gateway...");
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

        std::vector<uint8_t> buf(kBufSize);

        for (;;)
        {
            std::string lenStr = read_line();
            if (lenStr.empty())
            {
                // Timeout ou linha vazia - não é erro, apenas continua
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
                
            int len = atoi(lenStr.c_str());
            if (len <= 0 || len > (int)kBufSize)
            {
                ESP_LOGW(TAG, "RX[UART BORDER<-GW] tamanho inválido: '%s' (len=%d)", lenStr.c_str(), len);
                continue;
            }

            // Lê JSON com timeout
            size_t got = 0;
            const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
            while (got < (size_t)len && xTaskGetTickCount() < deadline)
            {
                int r = uart_read_bytes(kUartNum, buf.data() + got, len - got, pdMS_TO_TICKS(20));
                if (r > 0)
                    got += r;
            }
            
            if (got < (size_t)len)
            {
                ESP_LOGW(TAG, "RX[UART BORDER<-GW] timeout ao ler %d bytes (recebido %zu)", len, got);
                continue;
            }

            std::string json(reinterpret_cast<char *>(buf.data()), len);
            Protocol::Packet pkt;
            if (!Protocol::parse(json, pkt))
            {
                ESP_LOGW(TAG, "RX[UART BORDER<-GW] falha ao parsear JSON (%d bytes)", len);
                continue;
            }

            ESP_LOGI(TAG, "RX[UART BORDER<-GW] from=%s dst=%s (%d bytes)",
                     pkt.route.src.c_str(), pkt.route.dst.c_str(), len);
            LedManager::blink(TrafficSource::UART); // LED pisca quando recebe do UART

            // Atualizar informações de rastreabilidade
            Protocol::Packet updated_pkt = pkt; // Cópia para modificar
            std::string current_node_id = BLETransport::node_id();
            uint64_t now_ms = esp_timer_get_time() / 1000ULL;
            
            // Verificar se este node já está no path (detecção de loop)
            bool already_in_path = false;
            for (const auto &node : updated_pkt.trace.path)
            {
                if (node == current_node_id || node == "border")
                {
                    already_in_path = true;
                    break;
                }
            }
            
            // Se já passou por este node, descarta imediatamente (loop detectado)
            if (already_in_path)
            {
                ESP_LOGW(TAG, "Loop detectado: border node já está no path, descartando pacote");
                return;
            }
            
            // Verificar TTL antes de processar
            if (updated_pkt.ttl == 0)
            {
                ESP_LOGW(TAG, "TTL expirado, descartando pacote");
                return;
            }
            
            // Decrementa TTL e adiciona node ao path
            updated_pkt.ttl--;
            updated_pkt.trace.path.push_back(current_node_id);
            updated_pkt.trace.hop_count++;
            
            updated_pkt.trace.received_at_ms = now_ms;
            
            // Adicionar hop ao histórico
            Protocol::HopInfo hop;
            hop.node_id = current_node_id;
            hop.node_name = ""; // Pode ser preenchido pelo microserviço
            hop.timestamp_ms = now_ms;
            hop.transport = "UART";
            updated_pkt.trace.hop_history.push_back(hop);

            // Processa TOKEN (modo teste) - token do gateway via UART
#ifdef CONFIG_WETZEL_TEST_MODE
            if (updated_pkt.type == Protocol::PacketType::EVENT && updated_pkt.method == "TOKEN")
            {
                // Token do gateway - passa para a mesh
                if (updated_pkt.route.dst == "border" || updated_pkt.route.dst.empty())
                {
                    // Se não tem destino específico, passa para primeiro vizinho ou processa localmente
                    ESP_LOGI(TAG, "TOKEN recebido do Gateway via UART - processando...");
                    if (s_rx_handler)
                        s_rx_handler(updated_pkt);
                    return;
                }
            }
#endif

            // Reencaminha TODOS os pacotes recebidos do UART para a rede mesh
            // Border node aguarda 1 segundo antes de repassar (com LED aceso)
            if (updated_pkt.route.dst == "border" || updated_pkt.route.dst == "broadcast" || updated_pkt.route.dst.empty())
            {
                // Border node recebeu dado do UART - mantém LED aceso durante 1 segundo antes de repassar
                ESP_LOGI(TAG, "Border node %s recebeu dado do UART (hop %u) - mantendo LED aceso por 1 segundo antes de repassar...", 
                         current_node_id.c_str(), updated_pkt.trace.hop_count);
                LedManager::set_led_on_for_duration(1000); // Mantém LED aceso por 1 segundo
                vTaskDelay(pdMS_TO_TICKS(1000)); // Aguarda 1 segundo
                
                ESP_LOGI(TAG, "→ Reencaminhando pacote UART->MESH após 1 segundo: method=%s", updated_pkt.method.c_str());
                ESPNOWTransport::send(updated_pkt);
                LedManager::blink(TrafficSource::MESH); // LED pisca quando envia para a rede
            }
            
            pkt = updated_pkt; // Atualizar pkt original para o handler

            if (s_rx_handler)
                s_rx_handler(pkt);
        }
    }

    // ---------------------------------------------------------------------
    static void handshake_retry_task(void *)
    {
        for (;;)
        {
            if (!s_enabled)
            {
                bool ok = BorderUart::do_handshake(2000);
                if (ok)
                {
                    s_enabled = true;
                    LedManager::set_uart_enabled(true);
                    xTaskCreatePinnedToCore(BorderUart::uart_listen_task, "border_uart_rx", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
                    vTaskDelete(nullptr);
                }
                else
                {
                    LedManager::set_uart_enabled(false);
                    ESP_LOGW(TAG, "Handshake UART sem resposta; nova tentativa em 1s.");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            else
                vTaskDelete(nullptr);
        }
    }

} // namespace WetzelMesh
