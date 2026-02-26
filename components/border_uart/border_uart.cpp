#include "border_uart.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "protocol.hpp"
#include "led_manager.hpp"
#include "ble_transport.hpp"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "espnow_transport.hpp"
#include <vector>
#include <cstdlib>

namespace NetworkMesh
{
    static const char *TAG = "BORDER_UART";

    static constexpr uart_port_t kUartNum = UART_NUM_1;
    static constexpr int kTxPin = 15;
    static constexpr int kRxPin = 13;
    static constexpr int kBaud = 115200;
    static constexpr size_t kBufSize = 2048;

    static bool s_enabled = false;
    static BorderUart::RxHandler s_rx_handler = nullptr;

    static void handshake_retry_task(void *);

    bool BorderUart::init()
    {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "CONFIGURANDO UART (BORDER NODE)");
        ESP_LOGI(TAG, "   UART_NUM: %d", kUartNum);
        ESP_LOGI(TAG, "   TX Pin: %d -> Gateway RX (GPIO16)", kTxPin);
        ESP_LOGI(TAG, "   RX Pin: %d <- Gateway TX (GPIO17)", kRxPin);
        ESP_LOGI(TAG, "   Baud Rate: %d", kBaud);
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

        uart_config_t cfg{};
        cfg.baud_rate = kBaud;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

        uart_driver_delete(kUartNum);

        esp_err_t err = uart_param_config(kUartNum, &cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao configurar parametros UART: %s", esp_err_to_name(err));
            return false;
        }

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

        vTaskDelay(pdMS_TO_TICKS(500));

        LedManager::set_uart_enabled(false);
        xTaskCreatePinnedToCore(handshake_retry_task, "border_uart_hs", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
        vTaskDelay(pdMS_TO_TICKS(2000));
        return true;
    }

    bool BorderUart::is_enabled() { return s_enabled; }
    void BorderUart::set_rx_handler(RxHandler cb) { s_rx_handler = cb; }

    bool BorderUart::uart_write_json(const std::string &json)
    {
        char header[16];
        int n = snprintf(header, sizeof(header), "%u\n", (unsigned)json.size());
        if (n <= 0 || n >= (int)sizeof(header))
        {
            ESP_LOGE(TAG, "TX erro ao formatar header (n=%d)", n);
            return false;
        }

        int w1 = uart_write_bytes(kUartNum, header, n);
        if (w1 < 0 || w1 != n)
        {
            ESP_LOGE(TAG, "TX erro ao escrever header (w1=%d, esperado=%d)", w1, n);
            return false;
        }

        int w2 = uart_write_bytes(kUartNum, json.c_str(), json.size());
        if (w2 < 0 || w2 != (int)json.size())
        {
            ESP_LOGE(TAG, "TX erro ao escrever JSON (w2=%d, esperado=%zu)", w2, json.size());
            return false;
        }

        uart_wait_tx_done(kUartNum, pdMS_TO_TICKS(1000));
        return true;
    }

    bool BorderUart::send_to_gateway(const Protocol::Packet &pkt)
    {
        if (!s_enabled)
            return false;
        std::string json = Protocol::serialize(pkt);
        ESP_LOGI(TAG, "TX[BORDER->GW] %s -> %s (%u bytes)",
                 pkt.route.src.c_str(), pkt.route.dst.c_str(), (unsigned)json.size());
        BorderUart::uart_write_json(json);
        LedManager::blink(TrafficSource::UART);
        return true;
    }

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

            ESP_LOGI(TAG, "Enviando PING para gateway (tentativa %d/%d)...", retry + 1, kMaxRetries);

            Protocol::Packet ping{};
            ping.type = Protocol::PacketType::EVENT;
            ping.method = "PING";
            ping.route.src = "border";
            ping.route.dst = "gateway";
            ping.body = "{}";

            std::string json = Protocol::serialize(ping);

            if (!BorderUart::uart_write_json(json))
            {
                ESP_LOGE(TAG, "Falha ao enviar PING!");
                continue;
            }

            ESP_LOGI(TAG, "PING enviado! Aguardando PONG...");

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

            const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
            while (xTaskGetTickCount() < deadline)
            {
                std::string lenStr = read_line();
                if (lenStr.empty())
                {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                int len = atoi(lenStr.c_str());
                if (len <= 0 || len > (int)kBufSize)
                    continue;

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
                    break;

                Protocol::Packet pkt;
                std::string body(reinterpret_cast<char *>(jsonBuf.data()), len);

                if (!Protocol::parse(body, pkt))
                    continue;

                if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "PONG")
                {
                    ESP_LOGI(TAG, "Handshake OK! PONG recebido do gateway!");
                    return true;
                }
            }

            ESP_LOGW(TAG, "Handshake timeout (tentativa %d/%d)", retry + 1, kMaxRetries);
        }

        ESP_LOGE(TAG, "Handshake falhou apos %d tentativas", 3);
        return false;
    }

    void BorderUart::uart_listen_task(void *)
    {
        ESP_LOGI(TAG, "UART listen task iniciada");
        std::string acc;
        acc.reserve(16);

        auto read_line = [&]() -> std::string
        {
            acc.clear();
            const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
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
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            int len = atoi(lenStr.c_str());
            if (len <= 0 || len > (int)kBufSize)
            {
                ESP_LOGW(TAG, "RX tamanho invalido: '%s' (len=%d)", lenStr.c_str(), len);
                continue;
            }

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
                ESP_LOGW(TAG, "RX timeout ao ler %d bytes (recebido %zu)", len, got);
                continue;
            }

            std::string json(reinterpret_cast<char *>(buf.data()), len);
            Protocol::Packet pkt;
            if (!Protocol::parse(json, pkt))
            {
                ESP_LOGW(TAG, "RX falha ao parsear JSON (%d bytes)", len);
                continue;
            }

            ESP_LOGI(TAG, "RX[GW->BORDER] from=%s dst=%s (%d bytes)",
                     pkt.route.src.c_str(), pkt.route.dst.c_str(), len);
            LedManager::blink(TrafficSource::UART);

            Protocol::Packet updated_pkt = pkt;
            std::string current_node_id = BLETransport::node_id();
            uint64_t now_ms = esp_timer_get_time() / 1000ULL;

            bool already_in_path = false;
            for (const auto &node : updated_pkt.trace.path)
            {
                if (node == current_node_id || node == "border")
                {
                    already_in_path = true;
                    break;
                }
            }

            if (already_in_path)
            {
                ESP_LOGW(TAG, "Loop detectado: border node ja esta no path, descartando");
                continue;
            }

            if (updated_pkt.ttl == 0)
            {
                ESP_LOGW(TAG, "TTL expirado, descartando pacote");
                continue;
            }

            updated_pkt.ttl--;
            updated_pkt.trace.path.push_back(current_node_id);
            updated_pkt.trace.hop_count++;
            updated_pkt.trace.received_at_ms = now_ms;

            Protocol::HopInfo hop;
            hop.node_id = current_node_id;
            hop.timestamp_ms = now_ms;
            hop.transport = "UART";
            updated_pkt.trace.hop_history.push_back(hop);

            pkt = updated_pkt;

            if (s_rx_handler)
            {
                ESP_LOGI(TAG, "Processando pacote localmente: method=%s", updated_pkt.method.c_str());
                s_rx_handler(pkt);
            }

            if (updated_pkt.route.dst == "border" || updated_pkt.route.dst == "broadcast" || updated_pkt.route.dst.empty() ||
                (updated_pkt.type == Protocol::PacketType::REQUEST && updated_pkt.method == "DISCOVERY"))
            {
                ESP_LOGI(TAG, "Border %s reencaminhando UART->MESH: method=%s",
                         current_node_id.c_str(), updated_pkt.method.c_str());
                LedManager::set_led_on_for_duration(1000);
                vTaskDelay(pdMS_TO_TICKS(1000));
                ESPNOWTransport::send(updated_pkt);
            }
        }
    }

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
                    xTaskCreatePinnedToCore(BorderUart::uart_listen_task, "border_uart_rx", 8192, nullptr, 5, nullptr, tskNO_AFFINITY);
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

}
