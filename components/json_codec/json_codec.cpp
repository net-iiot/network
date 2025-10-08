#include "json_codec.hpp"
#include "cJSON.h"

namespace WetzelMesh
{
    std::string JSONCodec::wrap(const std::string &type, const std::string &payload)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", type.c_str());

        cJSON *parsed = cJSON_Parse(payload.c_str());
        if (parsed)
            cJSON_AddItemToObject(root, "payload", parsed);
        else
            cJSON_AddStringToObject(root, "payload", payload.c_str());

        char *printed = cJSON_PrintUnformatted(root);
        std::string out = printed ? printed : "{}";
        if (printed)
            cJSON_free(printed);
        cJSON_Delete(root);
        return out;
    }

    bool JSONCodec::unwrap(const std::string &json, std::string &type, std::string &payload)
    {
        cJSON *root = cJSON_Parse(json.c_str());
        if (!root)
            return false;

        auto cleanup = [&]()
        { cJSON_Delete(root); };
        cJSON *jtype = cJSON_GetObjectItem(root, "type");
        cJSON *jpay = cJSON_GetObjectItem(root, "payload");
        if (!cJSON_IsString(jtype) || !jpay)
        {
            cleanup();
            return false;
        }

        type = jtype->valuestring;

        if (cJSON_IsString(jpay))
            payload = jpay->valuestring;
        else
        {
            char *printed = cJSON_PrintUnformatted(jpay);
            payload = printed ? printed : "{}";
            if (printed)
                cJSON_free(printed);
        }
        cleanup();
        return true;
    }
} // namespace WetzelMesh
