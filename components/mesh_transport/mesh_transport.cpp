#include "mesh_transport.hpp"
#include "esp_log.h"

namespace monimesh
{
    static const char *TAG = "MeshTransport";

    esp_err_t MeshTransport::init()
    {
        ESP_LOGI(TAG, "MeshTransport initialized (placeholder)");
        return ESP_OK;
    }

    void MeshTransport::process()
    {
        ESP_LOGD(TAG, "Processing BLE Mesh events (placeholder)");
    }
}
