#include "gateway.hpp"
#include "esp_log.h"
#include "driver/uart.h"
#include "protocol.hpp"
#include "network_manager.hpp"
#include "esp_bt.h"
#include "led_manager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace WetzelMesh
{

    static const char *TAG = "GATEWAY";

    static constexpr uart_port_t kUartNum = UART_NUM_1;
    static constexpr int kTxPin = 17; // TX -> RX do nó-borda
    static constexpr int kRxPin = 16; // RX <- TX do nó-borda
    static constexpr int kBaud = 115200;
    static constexpr size_t kBufSize = 2048;

    // Só a test task permanece como função livre
    static void test_packet_task(void *);

    // ---------------------------------------------------------------------
    // Mantém método privado conforme seu header; não expomos fora da classe
    bool Gateway::uart_write_json(const std::string &json)
    {
        char header[16];
        int n = snprintf(header, sizeof(header), "%u\n", (unsigned)json.size());
        if (n <= 0)
            return false;

        int w1 = uart_write_bytes(kUartNum, header, n);
        int w2 = uart_write_bytes(kUartNum, json.c_str(), json.size());
        if (w1 < 0 || w2 < 0)
        {
            ESP_LOGE(TAG, "TX[UART GW->BORDER] erro driver (w1=%d w2=%d)", w1, w2);
            return false;
        }
        return true;
    }

    // Auxiliar local
    static bool uart_read_exact(size_t len, std::string &out)
    {
        out.resize(len);
        size_t got = 0;
        while (got < len)
        {
            int r = uart_read_bytes(kUartNum, reinterpret_cast<uint8_t *>(&out[got]),
                                    len - got, pdMS_TO_TICKS(100));
            if (r > 0)
                got += r;
            else
                vTaskDelay(pdMS_TO_TICKS(5));
        }
        return (got == len);
    }

    // ---------------------------------------------------------------------
    void Gateway::init()
    {
        // Liberar memória BT (não usamos)
        esp_err_t r = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        if (r == ESP_OK)
            ESP_LOGI(TAG, "Memoria BT liberada.");
        else
            ESP_LOGW(TAG, "BT ja liberado ou ativo.");

        // UART para comunicação com o nó-borda
        uart_config_t cfg{};
        cfg.baud_rate = kBaud;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

        ESP_ERROR_CHECK(uart_param_config(kUartNum, &cfg));
        ESP_ERROR_CHECK(uart_set_pin(kUartNum, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(kUartNum, kBufSize, kBufSize, 0, nullptr, 0));

        // RX pela UART (GW <- Nó)
        xTaskCreatePinnedToCore(uart_listen_task, "gw_uart_rx", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);

        // Gera tráfego de teste HELLO → valida caminho completo UART→MESH
        xTaskCreatePinnedToCore(test_packet_task, "gw_uart_tx_test", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);

        ESP_LOGI(TAG, "Gateway inicializado (UART TX=17 RX=16, BLE/BT off).");
    }

    void Gateway::uart_listen_task(void *)
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

        for (;;)
        {
            std::string lenStr = read_line();
            if (lenStr.empty())
                continue;

            int len = atoi(lenStr.c_str());
            if (len <= 0 || len > (int)kBufSize)
                continue;

            std::string json;
            if (!uart_read_exact((size_t)len, json))
                continue;

            Protocol::Packet pkt;
            if (!Protocol::parse(json, pkt))
                continue;

            ESP_LOGI(TAG, "RX[UART GW<-BORDER] from=%s dst=%s (%d bytes)",
                     pkt.route.src.c_str(), pkt.route.dst.c_str(), len);
            LedManager::blink(TrafficSource::UART);

            if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "PING")
            {
                Protocol::Packet pong{};
                pong.type = Protocol::PacketType::EVENT;
                pong.method = "PONG";
                pong.route.src = "gateway";
                pong.route.dst = pkt.route.src;
                pong.body = R"({"status":"alive"})";

                ESP_LOGI(TAG, "TX[UART GW->BORDER] PONG para %s", pong.route.dst.c_str());
                uart_write_json(Protocol::serialize(pong));
                continue;
            }

            if (pkt.route.dst == "server")
            {
                ESP_LOGI(TAG, "Encaminharia pacote ao servidor (stub).");
                LedManager::blink(TrafficSource::SERVER);
            }
        }
    }

    static void test_packet_task(void *)
    {
        for (;;)
        {
            Protocol::Packet pkt{};
            pkt.type = Protocol::PacketType::EVENT;
            pkt.method = "HELLO";
            pkt.route.src = "gateway";
            pkt.route.dst = "border";
            pkt.body = R"({"msg":"from_gateway"})";

            // ⚠️ Usa API pública, mantendo uart_write_json privada:
            Gateway::send(pkt);

            vTaskDelay(pdMS_TO_TICKS(1000)); // a cada 1 s
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

} // namespace WetzelMesh
