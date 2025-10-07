#include "security.hpp"
#include "esp_log.h"

namespace wetzelmesh {
static const char* TAG = "Security";

std::vector<uint8_t> Security::hmacSha256(const std::string& key, const std::string& data) {
    ESP_LOGI(TAG, "Calculating HMAC-SHA256 (placeholder)");
    std::vector<uint8_t> result(32, 0xAA);
    return result;
}
}
