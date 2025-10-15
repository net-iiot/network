#include "border_uart.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "protocol.hpp"
#include "led_manager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <vector>
#include <cstdlib>

namespace WetzelMesh
{

    static const char *TAG = "BORDER_UART";

    // Ajuste conforme o cabeamento no nó-borda:
    static constexpr uart_port_t kUartNum = UART_NUM_1;
    static constexpr int kTxPin = 16; // TX do nó-borda → RX do gateway
    static constexpr int kRxPin = 17; // RX do nó-borda ← TX do gateway
    static constexpr int kBaud = 115200;
    static constexpr size_t kBufSize = 2048;

    static bool s_enabled = false;
    static BorderUart::RxHandler s_rx_handler = nullptr;

    // Forward local
    static void handshake_retry_task(void *);

    bool BorderUart::init()
    {
        if (s_enabled)
            return true;

        uart_config_t cfg{};
        cfg.baud_rate = kBaud;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

        esp_err_t err = uart_param_config(kUartNum, &cfg);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "uart_param_config: %s", esp_err_to_name(err));
            LedManager::set_uart_enabled(false); // LED26 ON (sem link)
            return false;
        }

        err = uart_set_pin(kUartNum, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "uart_set_pin: %s", esp_err_to_name(err));
            LedManager::set_uart_enabled(false);
            return false;
        }

        err = uart_driver_install(kUartNum, kBufSize, kBufSize, 0, nullptr, 0);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "uart_driver_install: %s", esp_err_to_name(err));
            LedManager::set_uart_enabled(false);
            return false;
        }

        // Estado inicial: sem link => LED 26 aceso
        s_enabled = false;
        LedManager::set_uart_enabled(false);

        // Tarefa que fica tentando handshake de 1 em 1 segundo
        xTaskCreatePinnedToCore(handshake_retry_task, "border_uart_hs", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
        ESP_LOGI(TAG, "UART configurada; aguardando handshake (PING->PONG).");
        return true;
    }

    bool BorderUart::is_enabled() { return s_enabled; }
    void BorderUart::set_rx_handler(RxHandler cb) { s_rx_handler = cb; }

    bool BorderUart::send_to_gateway(const Protocol::Packet &pkt)
    {
        if (!s_enabled)
            return false;
        std::string json = Protocol::serialize(pkt);
        ESP_LOGI(TAG, "TX[UART BORDER->GW] %s -> %s (%u bytes)",
                 pkt.route.src.c_str(), pkt.route.dst.c_str(), (unsigned)json.size());
        bool ok = uart_write_json(json);
        if (ok)
        {
            LedManager::blink(TrafficSource::UART); // Pisca LED 26 no TX
        }
        return ok;
    }

    bool BorderUart::uart_write_json(const std::string &json)
    {
        char header[16];
        int n = snprintf(header, sizeof(header), "%u\n", (unsigned)json.size());
        if (n <= 0)
            return false;

        int w1 = uart_write_bytes(kUartNum, header, n);
        int w2 = uart_write_bytes(kUartNum, json.c_str(), json.size());
        if (w1 < 0 || w2 < 0)
        {
            ESP_LOGE(TAG, "TX[UART BORDER->GW] erro driver (w1=%d w2=%d)", w1, w2);
            return false;
        }
        return true;
    }

    bool BorderUart::do_handshake(unsigned timeout_ms)
    {
        // Envia um PACKET "PING" no framing <len>\n<json> e espera "PONG".
        Protocol::Packet ping{};
        ping.type = Protocol::PacketType::EVENT;
        ping.method = "PING";
        ping.route.src = "border";
        ping.route.dst = "gateway";
        ping.body = "{}";

        std::string json = Protocol::serialize(ping);

        // Flush do RX antes do PING
        uint8_t tmp[64];
        while (uart_read_bytes(kUartNum, tmp, sizeof(tmp), 0) > 0)
        { /* flush */
        }

        // Envia PING
        if (!uart_write_json(json))
            return false;

        // Aguarda PONG (bloqueante, simples)
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

        std::vector<uint8_t> buf(kBufSize);

        std::string lenStr = read_line();
        if (lenStr.empty())
            return false;

        int len = atoi(lenStr.c_str());
        if (len <= 0 || len > (int)buf.size())
            return false;

        int got = 0;
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
        while (got < len && xTaskGetTickCount() < deadline)
        {
            int r = uart_read_bytes(kUartNum, buf.data() + got, len - got, pdMS_TO_TICKS(50));
            if (r > 0)
                got += r;
        }
        if (got != len)
            return false;

        std::string rxJson((char *)buf.data(), len);
        Protocol::Packet resp;
        if (!Protocol::parse(rxJson, resp))
            return false;

        bool ok = (resp.type == Protocol::PacketType::EVENT && resp.method == "PONG");
        if (ok)
            ESP_LOGI(TAG, "Handshake OK (PONG do gateway).");
        return ok;
    }

    void BorderUart::uart_listen_task(void *)
    {
        std::string acc;
        acc.reserve(32);

        auto read_line = [&]() -> std::string
        {
            acc.clear();
            while (true)
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
                {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            return acc;
        };

        std::vector<uint8_t> jsonBuf(kBufSize);

        for (;;)
        {
            std::string lenStr = read_line();
            if (lenStr.empty())
                continue;

            int len = atoi(lenStr.c_str());
            if (len <= 0 || len > (int)jsonBuf.size())
            {
                ESP_LOGW(TAG, "RX[UART BORDER<-GW] len invalido: %d", len);
                continue;
            }

            int got = 0;
            while (got < len)
            {
                int r = uart_read_bytes(kUartNum, jsonBuf.data() + got, len - got, pdMS_TO_TICKS(100));
                if (r > 0)
                    got += r;
                else
                    vTaskDelay(pdMS_TO_TICKS(5));
            }

            std::string json((char *)jsonBuf.data(), len);
            Protocol::Packet pkt;
            if (Protocol::parse(json, pkt))
            {
                ESP_LOGI(TAG, "RX[UART BORDER<-GW] from=%s dst=%s (%d bytes)",
                         pkt.route.src.c_str(), pkt.route.dst.c_str(), len);

                LedManager::blink(TrafficSource::UART); // Pisca LED 26 no RX

                if (s_rx_handler)
                    s_rx_handler(pkt);
            }
            else
            {
                ESP_LOGW(TAG, "RX[UART BORDER<-GW] JSON invalido");
            }
        }
    }

    // Tarefa que tenta handshake periodicamente até conectar
    static void handshake_retry_task(void *)
    {
        for (;;)
        {
            if (!s_enabled)
            {
                bool ok = BorderUart::do_handshake(1500);
                if (ok)
                {
                    s_enabled = true;
                    LedManager::set_uart_enabled(true); // LED26 OFF (link ativo)
                    // Sobe a tarefa de RX e sai do retry
                    xTaskCreatePinnedToCore(BorderUart::uart_listen_task, "border_uart_rx", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
                    vTaskDelete(nullptr);
                }
                else
                {
                    // Mantém LED 26 ON; tenta de novo em 1s
                    LedManager::set_uart_enabled(false);
                    ESP_LOGW(TAG, "Handshake UART sem resposta; nova tentativa em 1s.");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            else
            {
                vTaskDelete(nullptr);
            }
        }
    }

} // namespace WetzelMesh
