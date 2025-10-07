#include "cbor_codec.hpp"
#include "esp_log.h"

namespace monimesh {
static const char* TAG = "CborCodec";

esp_err_t CborCodec::encode(const std::string& json, std::vector<uint8_t>& out) {
    ESP_LOGI(TAG, "Encoding JSON to CBOR (placeholder)");
    out.assign(json.begin(), json.end());
    return ESP_OK;
}

esp_err_t CborCodec::decode(const std::vector<uint8_t>& data, std::string& json_out) {
    ESP_LOGI(TAG, "Decoding CBOR to JSON (placeholder)");
    json_out.assign(data.begin(), data.end());
    return ESP_OK;
}
}
