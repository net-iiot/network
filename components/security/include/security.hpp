#pragma once
#include <string>
#include <vector>
#include "esp_err.h"

namespace wetzelmesh {
class Security {
public:
    static std::vector<uint8_t> hmacSha256(const std::string& key, const std::string& data);
};
}
