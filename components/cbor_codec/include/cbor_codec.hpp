#pragma once
#include <vector>
#include <string>
#include "esp_err.h"

namespace monimesh {
class CborCodec {
public:
    static esp_err_t encode(const std::string& json, std::vector<uint8_t>& out);
    static esp_err_t decode(const std::vector<uint8_t>& data, std::string& json_out);
};
}
