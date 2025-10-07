#include "json_codec.hpp"
#include "esp_log.h"

namespace wetzelmesh
{

    static const char *TAG = "JSONCodec";

    std::string JSONCodec::encode(const char *type, const char *payload)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", type);
        cJSON_AddStringToObject(root, "payload", payload);

        char *json_str = cJSON_PrintUnformatted(root);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);

        ESP_LOGI(TAG, "Encoded JSON: %s", result.c_str());
        return result;
    }

    bool JSONCodec::decode(const std::string &json, std::string &type, std::string &payload)
    {
        cJSON *root = cJSON_Parse(json.c_str());
        if (!root)
        {
            ESP_LOGE(TAG, "Failed to parse JSON");
            return false;
        }

        cJSON *type_item = cJSON_GetObjectItem(root, "type");
        cJSON *payload_item = cJSON_GetObjectItem(root, "payload");

        if (cJSON_IsString(type_item) && cJSON_IsString(payload_item))
        {
            type = type_item->valuestring;
            payload = payload_item->valuestring;
            cJSON_Delete(root);
            return true;
        }

        cJSON_Delete(root);
        ESP_LOGE(TAG, "Invalid JSON format");
        return false;
    }

} // namespace monimesh
