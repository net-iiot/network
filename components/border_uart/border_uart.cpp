#include "border_uart.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "protocol.hpp"
#include "led_manager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "espnow_transport.hpp"
#include <vector>
#include <cstdlib>

namespace WetzelMesh
{
    static const char *TAG = "BORDER_UART";

    static constexpr uart_port_t kUartNum = UART_NUM_1;
    static constexpr int kTxPin = 13; // TX -> RX do Gateway (GPIO16)
    static constexpr int kRxPin = 15; // RX <- TX do Gateway (GPIO17)
    static constexpr int kBaud = 115200;
    static constexpr size_t kBufSize = 2048;

    static bool s_enabled = false;
    static BorderUart::RxHandler s_rx_handler = nullptr;

    static void handshake_retry_task(void *);

    bool BorderUart::init()
    {
        uart_config_t cfg{};
        cfg.baud_rate = kBaud;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

        ESP_ERROR_CHECK(uart_param_config(kUartNum, &cfg));
        ESP_ERROR_CHECK(uart_set_pin(kUartNum, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(kUartNum, kBufSize, kBufSize, 0, nullptr, 0));

        LedManager::set_uart_enabled(false);
        xTaskCreatePinnedToCore(handshake_retry_task, "border_uart_hs", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
        ESP_LOGI(TAG, "UART iniciada (TX=13 RX=15); aguardando handshake com Gateway...");
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
        if (n <= 0)
            return false;
        uart_write_bytes(kUartNum, header, n);
        uart_write_bytes(kUartNum, json.c_str(), json.size());
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
            
            Protocol::Packet ping{};
            ping.type = Protocol::PacketType::EVENT;
            ping.method = "PING";
            ping.route.src = "border";
            ping.route.dst = "gateway";
            ping.body = "{}";

            std::string json = Protocol::serialize(ping);
            BorderUart::uart_write_json(json);

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
                continue;
            int len = atoi(lenStr.c_str());
            if (len <= 0 || len > (int)kBufSize)
                continue;

            std::vector<uint8_t> jsonBuf(len);
            size_t got = 0;
            while (got < (size_t)len)
            {
                int r = uart_read_bytes(kUartNum, jsonBuf.data() + got, len - got, pdMS_TO_TICKS(20));
                if (r > 0)
                    got += r;
            }

            Protocol::Packet pkt;
            std::string body(reinterpret_cast<char *>(jsonBuf.data()), len);
            if (!Protocol::parse(body, pkt))
                continue;

            if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "PONG")
            {
                ESP_LOGI(TAG, "Handshake OK (PONG recebido)");
                return true;
            }
        }
        
        // Se chegou aqui, timeout sem receber PONG
        ESP_LOGW(TAG, "Handshake timeout (tentativa %d/%d)", retry + 1, kMaxRetries);
        }
        
        ESP_LOGE(TAG, "Handshake falhou após %d tentativas", kMaxRetries);
        return false;
    }

    // ---------------------------------------------------------------------
    void BorderUart::uart_listen_task(void *)
    {
        std::string acc;
        acc.reserve(16);

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
                    vTaskDelay(pdMS_TO_TICKS(5));
            }
            return acc;
        };

        std::vector<uint8_t> buf(kBufSize);

        for (;;)
        {
            std::string lenStr = read_line();
            if (lenStr.empty())
                continue;
            int len = atoi(lenStr.c_str());
            if (len <= 0 || len > (int)kBufSize)
                continue;

            size_t got = 0;
            while (got < (size_t)len)
            {
                int r = uart_read_bytes(kUartNum, buf.data() + got, len - got, pdMS_TO_TICKS(20));
                if (r > 0)
                    got += r;
            }

            std::string json(reinterpret_cast<char *>(buf.data()), len);
            Protocol::Packet pkt;
            if (!Protocol::parse(json, pkt))
                continue;

            ESP_LOGI(TAG, "RX[UART BORDER<-GW] from=%s dst=%s (%d bytes)",
                     pkt.route.src.c_str(), pkt.route.dst.c_str(), len);
            LedManager::blink(TrafficSource::UART);

            // Processa TOKEN (modo teste) - token do gateway via UART
#ifdef CONFIG_WETZEL_TEST_MODE
            if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "TOKEN")
            {
                // Token do gateway - passa para a mesh
                if (pkt.route.dst == "border" || pkt.route.dst.empty())
                {
                    // Se não tem destino específico, passa para primeiro vizinho ou processa localmente
                    ESP_LOGI(TAG, "TOKEN recebido do Gateway via UART - processando...");
                    if (s_rx_handler)
                        s_rx_handler(pkt);
                    return;
                }
            }
#endif

            // Reencaminha para a malha usando SEU módulo real:
            if (pkt.method == "HELLO")
            {
                ESP_LOGI(TAG, "→ Enviando HELLO para a rede ESP-NOW...");
                ESPNOWTransport::send(pkt);
                LedManager::blink(TrafficSource::MESH);
            }

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
