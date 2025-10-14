#include "border_uart.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "protocol.hpp"
#include "led_manager.hpp"
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
            return false;
        }

        err = uart_set_pin(kUartNum, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "uart_set_pin: %s", esp_err_to_name(err));
            return false;
        }

        err = uart_driver_install(kUartNum, kBufSize, kBufSize, 0, nullptr, 0);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "uart_driver_install: %s", esp_err_to_name(err));
            return false;
        }

        xTaskCreatePinnedToCore(uart_listen_task, "border_uart_rx", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);

        s_enabled = true;
        ESP_LOGI(TAG, "BORDER UART ativa (TX=%d RX=%d, %dbps)", kTxPin, kRxPin, kBaud);
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
        return uart_write_json(json);
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
            ESP_LOGE(TAG, "TX[UART BORDER->GW] erro driver");
            return false;
        }
        return true;
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
                LedManager::on_packet_received();
                if (s_rx_handler)
                    s_rx_handler(pkt);
            }
            else
            {
                ESP_LOGW(TAG, "RX[UART BORDER<-GW] JSON invalido");
            }
        }
    }

} // namespace WetzelMesh
