#include "interpreter_registry.hpp"
#include "esp_log.h"

namespace monimesh {
static const char* TAG = "TelemetryV1";

static void telemetryHandler(const Message& msg) {
    ESP_LOGI(TAG, "Telemetry received: %s", msg.payload.c_str());
}

__attribute__((constructor))
static void registerTelemetry() {
    InterpreterRegistry::instance().registerHandler(1, telemetryHandler);
}
}
