#include "uart_bridge.hpp"
#include "esp_log.h"
#include "driver/uart.h"

namespace monimesh {
static const char* TAG = "UARTBridge";

esp_err_t UARTBridge::init() {
    ESP_LOGI(TAG, "UARTBridge initialized (placeholder)");
    // Configuração mínima UART (não abre porta ainda)
    return ESP_OK;
}

esp_err_t UARTBridge::send(const uint8_t* data, size_t len) {
    ESP_LOGD(TAG, "UART send len=%d", (int)len);
    return ESP_OK;
}

int UARTBridge::read(uint8_t* buffer, size_t max_len) {
    ESP_LOGD(TAG, "UART read (placeholder)");
    return 0;
}
}
