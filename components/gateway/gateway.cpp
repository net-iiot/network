#include "gateway.hpp"
#include "esp_log.h"
#include "driver/uart.h"
#include "protocol.hpp"
#include "network_manager.hpp"
#include "esp_bt.h"
#include "led_manager.hpp" // Inclui LedManager e TrafficSource

namespace WetzelMesh
{

    static const char *TAG = "GATEWAY";

    // Ajuste conforme seu hardware
    static constexpr uart_port_t kUartNum = UART_NUM_1;
    static constexpr int kTxPin = 17;
    static constexpr int kRxPin = 16;
    static constexpr int kBaud = 115200;
    static constexpr size_t kBufSize = 2048;

    void Gateway::init()
    {
        // Desativa BLE/BT e libera RAM
        esp_err_t r = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        if (r == ESP_OK)
        {
            ESP_LOGI(TAG, "Memoria BT liberada.");
        }
        else if (r == ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "BT ja liberado ou controlador ativo.");
        }
        else
        {
            ESP_LOGW(TAG, "Falha liberar BT: %s", esp_err_to_name(r));
        }

        // UART GW<->Borda
        uart_config_t cfg{};
        cfg.baud_rate = kBaud;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

        ESP_ERROR_CHECK(uart_param_config(kUartNum, &cfg));
        ESP_ERROR_CHECK(uart_set_pin(kUartNum, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(kUartNum, kBufSize, kBufSize, 0, nullptr, 0));

        xTaskCreatePinnedToCore(uart_listen_task, "gw_uart_rx", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);

        ESP_LOGI(TAG, "Gateway inicializado (UART ativa, BLE/BT off).");
    }

    bool Gateway::send(const Protocol::Packet &pkt)
    {
        std::string json = Protocol::serialize(pkt);
        ESP_LOGI(TAG, "TX[SERVER] from=%s dst=%s (%u bytes)",
                 pkt.route.src.c_str(), pkt.route.dst.c_str(), (unsigned)json.size());
        // TODO: envio real ao servidor
        LedManager::blink(TrafficSource::SERVER); // Pisca LED p/ TX ao servidor
        return true;
    }

    bool Gateway::send_to_border(const Protocol::Packet &pkt)
    {
        std::string json = Protocol::serialize(pkt);
        ESP_LOGI(TAG, "TX[UART GW->BORDER] %s -> %s (%u bytes)",
                 pkt.route.src.c_str(), pkt.route.dst.c_str(), (unsigned)json.size());
        LedManager::blink(TrafficSource::UART); // Pisca LED p/ TX UART
        return uart_write_json(json);
    }

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
            ESP_LOGE(TAG, "TX[UART GW->BORDER] erro driver");
            return false;
        }
        return true;
    }

    void Gateway::uart_listen_task(void *)
    {
        std::string acc;
        acc.reserve(kBufSize);

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
                    acc.push_back((char)ch);
                    if (acc.size() > 12)
                        acc.erase(acc.begin()); // evita header absurdo
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
            // ------------- lê header <len>\n -------------
            std::string lenStr = read_line();
            if (lenStr.empty())
                continue;

            int len = atoi(lenStr.c_str());
            if (len <= 0 || len > (int)jsonBuf.size())
            {
                ESP_LOGW(TAG, "RX[UART GW<-BORDER] len invalido: %d", len);
                continue;
            }

            // ------------- lê corpo JSON -------------
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
            if (!Protocol::parse(json, pkt))
            {
                ESP_LOGW(TAG, "RX[UART GW<-BORDER] JSON invalido");
                continue;
            }

            ESP_LOGI(TAG, "RX[UART GW<-BORDER] from=%s dst=%s (%d bytes)",
                     pkt.route.src.c_str(), pkt.route.dst.c_str(), len);
            LedManager::blink(TrafficSource::UART); // Pisca LED p/ RX UART

            // --------- HANDSHAKE: responde somente se for PING ---------
            if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "PING")
            {
                Protocol::Packet pong{};
                pong.type = Protocol::PacketType::EVENT; // 👈 EVENT (não RESPONSE)
                pong.method = "PONG";
                pong.route.src = "gateway";
                pong.route.dst = pkt.route.src;
                pong.body = R"({"status":"ok"})";

                ESP_LOGI(TAG, "HANDSHAKE: recebido PING; enviando PONG a %s.", pong.route.dst.c_str());
                send_to_border(pong);
                continue; // não processa além disso
            }

            // --------- ROTEAMENTO (gateway não participa da mesh) ---------
            if (pkt.route.dst == "server")
            {
                // Sobe pro backend
                send(pkt);
            }
            else
            {
                // Qualquer outro destino não é roteado pelo gateway
                ESP_LOGI(TAG, "GW: destino nao-servidor (%s); nenhum roteamento aplicado.", pkt.route.dst.c_str());
            }
        }
    }

} // namespace WetzelMesh
