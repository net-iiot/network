#include "mqtt_gateway.hpp"
#include "esp_log.h"

namespace monimesh {
static const char* TAG = "MQTTGateway";

esp_err_t MQTTGateway::connect() {
    ESP_LOGI(TAG, "Connecting to MQTT broker (placeholder)");
    return ESP_OK;
}

esp_err_t MQTTGateway::publish(const std::string& topic, const std::string& msg) {
    ESP_LOGI(TAG, "Publish [%s]: %s", topic.c_str(), msg.c_str());
    return ESP_OK;
}
}
