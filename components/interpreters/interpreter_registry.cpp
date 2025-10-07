#include "interpreter_registry.hpp"
#include "esp_log.h"

namespace wetzelmesh {
static const char* TAG = "InterpreterRegistry";

InterpreterRegistry& InterpreterRegistry::instance() {
    static InterpreterRegistry reg;
    return reg;
}

void InterpreterRegistry::registerHandler(uint16_t map_id, Handler handler) {
    handlers[map_id] = handler;
    ESP_LOGI(TAG, "Handler registered for map_id=%u", map_id);
}

void InterpreterRegistry::handle(const Message& msg) {
    auto it = handlers.find(msg.map_id);
    if (it != handlers.end()) {
        ESP_LOGI(TAG, "Handling map_id=%u", msg.map_id);
        it->second(msg);
    } else {
        ESP_LOGW(TAG, "No handler for map_id=%u", msg.map_id);
    }
}
}
