#pragma once
#include "esp_err.h"

namespace monimesh {
class UARTBridge {
public:
    esp_err_t init();
    esp_err_t send(const uint8_t* data, size_t len);
    int read(uint8_t* buffer, size_t max_len);
};
}
