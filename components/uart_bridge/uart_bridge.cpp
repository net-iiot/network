#include "uart_bridge.hpp"
#include "esp_log.h"
#include "driver/uart.h"

namespace monimesh
{
    static const char *TAG = "UARTBridge";

// UART CONFIG
#define UART_PORT UART_NUM_2
#define UART_TX_PIN 13
#define UART_RX_PIN 15
#define UART_BAUD 115200
#define UART_BUF_SIZE (1024)

    esp_err_t UARTBridge::init()
    {
        ESP_LOGI(TAG, "UART init: TX=%d, RX=%d, Baud=%d", UART_TX_PIN, UART_RX_PIN, UART_BAUD);
        uart_config_t uart_config = {
            .baud_rate = UART_BAUD,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 0,
            .flags = 0,
        };

        ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        return ESP_OK;
    }

    esp_err_t UARTBridge::send(const uint8_t *data, size_t len)
    {
        int sent = uart_write_bytes(UART_PORT, (const char *)data, len);
        ESP_LOGD(TAG, "UART sent %d bytes", sent);
        return sent > 0 ? ESP_OK : ESP_FAIL;
    }

    int UARTBridge::read(uint8_t *buffer, size_t max_len)
    {
        int len = uart_read_bytes(UART_PORT, buffer, max_len, pdMS_TO_TICKS(10));
        if (len > 0)
            ESP_LOGD(TAG, "UART read %d bytes", len);
        return len;
    }
}
