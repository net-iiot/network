#include "gateway.hpp"
#include "esp_log.h"
#include "driver/uart.h"
#include "protocol.hpp"
#include "network_manager.hpp"
#include "esp_bt.h"
#include "led_manager.hpp"

namespace WetzelMesh
{

    static const char *TAG = "GATEWAY";

    // Ajuste conforme seu hardware (GW)
    static constexpr uart_port_t kUartNum = UART_NUM_1;
    static constexpr int kTxPin = 17; // TX do GATEWAY → RX do BORDA
    static constexpr int kRxPin = 16; // RX do GATEWAY ← TX do BORDA
    static constexpr int kBaud = 115200;
    static constexpr size_t kBufSize = 2048;

    // -----------------------------------------------------------------------------
    // Helpers locais
    // -----------------------------------------------------------------------------
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

    // -----------------------------------------------------------------------------
    // Métodos públicos
    // -----------------------------------------------------------------------------
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
            ESP_LOGW(TAG, "BT ja liberado ou ativo.");
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
        LedManager::blink(TrafficSource::SERVER); // Pisca LED (Server TX)
        return true;
    }

    bool Gateway::send_to_border(const Protocol::Packet &pkt)
    {
        std::string json = Protocol::serialize(pkt);
        ESP_LOGI(TAG, "TX[UART GW->BORDER] %s -> %s (%u bytes)",
                 pkt.route.src.c_str(), pkt.route.dst.c_str(), (unsigned)json.size());
        LedManager::blink(TrafficSource::UART); // Pisca LED (UART TX)
        return uart_write_json(json);
    }

    // -----------------------------------------------------------------------------
    // Task de RX pela UART (GW <- Borda)
    // -----------------------------------------------------------------------------
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
                {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            return acc;
        };

        for (;;)
        {
            // 1) Lê o header "<len>\n"
            std::string lenStr = read_line();
            if (lenStr.empty())
                continue;

            int len = atoi(lenStr.c_str());
            if (len <= 0 || len > (int)kBufSize)
            {
                ESP_LOGW(TAG, "RX[UART GW<-BORDER] len invalido: %d", len);
                continue;
            }

            // 2) Lê o corpo JSON com exatamente 'len' bytes
            std::string json;
            if (!uart_read_exact((size_t)len, json))
            {
                ESP_LOGW(TAG, "RX[UART GW<-BORDER] leitura incompleta");
                continue;
            }

            Protocol::Packet pkt;
            if (!Protocol::parse(json, pkt))
            {
                ESP_LOGW(TAG, "RX[UART GW<-BORDER] JSON invalido");
                continue;
            }

            ESP_LOGI(TAG, "RX[UART GW<-BORDER] from=%s dst=%s (%d bytes)",
                     pkt.route.src.c_str(), pkt.route.dst.c_str(), len);
            LedManager::blink(TrafficSource::UART); // RX UART

            // -----------------------
            // PROTOCOLOS ESPECIAIS
            // -----------------------
            // Handshake: se vier EVENT "PING" do nó-borda, responda EVENT "PONG"
            if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "PING")
            {
                Protocol::Packet pong{};
                pong.type = Protocol::PacketType::EVENT;
                pong.method = "PONG";
                pong.route.src = "gateway";
                pong.route.dst = pkt.route.src;
                pong.body = R"({"status":"alive"})";

                ESP_LOGI(TAG, "TX[UART GW->BORDER] PONG para %s", pong.route.dst.c_str());
                (void)send_to_border(pong);
                continue;
            }

            // Roteamento: se destino for "server", manda para o servidor
            if (pkt.route.dst == "server")
            {
                (void)send(pkt);
            }
            else
            {
                // Para outros destinos, poderia haver outra lógica
                ESP_LOGI(TAG, "GW roteamento local pendente p/ dst=%s", pkt.route.dst.c_str());
            }

            // Opcional: ACK como RESPONSE (confirmação adicional ao nó-borda)
            Protocol::Packet ack = Protocol::make_response(
                "gateway", pkt.route.src, 200, R"({"status":"received"})");
            (void)send_to_border(ack);
        }
    }

    // -----------------------------------------------------------------------------
    // Método privado
    // -----------------------------------------------------------------------------
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

} // namespace WetzelMesh
