#pragma once
#include "esp_err.h"
#include <string>

namespace wetzelmesh {
class MQTTGateway {
public:
    esp_err_t connect();
    esp_err_t publish(const std::string& topic, const std::string& msg);
};
}
