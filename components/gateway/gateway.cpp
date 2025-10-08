#include "gateway.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "router.hpp"

using namespace WetzelMesh;

static const char *TAG = "GATEWAY";

void Gateway::init()
{
    ESP_LOGI(TAG, "Inicializando UART do gateway...");

    uart_config_t cfg = {};
    cfg.baud_rate = BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE, BUF_SIZE, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreatePinnedToCore(&Gateway::listen_task, "uart_listen", 4096, nullptr, 5, nullptr, 1);
}

void Gateway::send(const Protocol::Packet &packet)
{
    std::string json = Protocol::serialize(packet);
    json.push_back('\n'); // framing simples (linha)
    uart_write_bytes(UART_PORT, json.c_str(), json.size());
    ESP_LOGI(TAG, "📤 UART -> servidor: %s", json.c_str());
}

void Gateway::listen_task(void *param)
{
    uint8_t buf[BUF_SIZE];
    std::string acc;

    while (true)
    {
        int len = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (len > 0)
        {
            acc.append((const char *)buf, len);
            // framing por linha
            size_t pos;
            while ((pos = acc.find('\n')) != std::string::npos)
            {
                std::string line = acc.substr(0, pos);
                acc.erase(0, pos + 1);

                Protocol::Packet packet;
                if (Protocol::parse(line, packet))
                {
                    ESP_LOGI(TAG, "📥 UART <- servidor: %s", line.c_str());
                    Router::handle_packet(packet);
                }
                else
                {
                    ESP_LOGW(TAG, "Linha UART inválida: %s", line.c_str());
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
