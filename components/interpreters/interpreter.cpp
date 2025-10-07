#include "interpreter.hpp"
#include "esp_log.h"

namespace wetzelmesh
{

    static const char *TAG = "Interpreter";

    void Interpreter::registerHandler(const std::string &target, const std::string &action, Handler h)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        handlers_[makeKey(target, action)] = std::move(h);
        ESP_LOGI(TAG, "handler registrado: target='%s' action='%s'", target.c_str(), action.c_str());
    }

    std::string Interpreter::makeKey(const std::string &target, const std::string &action) const
    {
        return target + "/" + action;
    }

    std::string Interpreter::buildOk(const char *target, const char *action, const cJSON *idField, const char *message, cJSON *data)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "ok");
        if (target)
            cJSON_AddStringToObject(root, "target", target);
        if (action)
            cJSON_AddStringToObject(root, "action", action);
        if (message)
            cJSON_AddStringToObject(root, "message", message);
        if (idField)
        { // mantém mesmo tipo de id (número ou string)
            if (cJSON_IsNumber(idField))
                cJSON_AddNumberToObject(root, "id", idField->valuedouble);
            else if (cJSON_IsString(idField))
                cJSON_AddStringToObject(root, "id", idField->valuestring);
        }
        if (data)
            cJSON_AddItemToObject(root, "data", data); // transfere ownership
        char *s = cJSON_PrintUnformatted(root);
        std::string out = s ? s : "{}";
        if (s)
            cJSON_free(s);
        cJSON_Delete(root);
        return out;
    }

    std::string Interpreter::buildErr(const char *target, const char *action, const cJSON *idField, const char *message)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "error");
        if (target)
            cJSON_AddStringToObject(root, "target", target);
        if (action)
            cJSON_AddStringToObject(root, "action", action);
        if (message)
            cJSON_AddStringToObject(root, "message", message);
        if (idField)
        {
            if (cJSON_IsNumber(idField))
                cJSON_AddNumberToObject(root, "id", idField->valuedouble);
            else if (cJSON_IsString(idField))
                cJSON_AddStringToObject(root, "id", idField->valuestring);
        }
        char *s = cJSON_PrintUnformatted(root);
        std::string out = s ? s : "{}";
        if (s)
            cJSON_free(s);
        cJSON_Delete(root);
        return out;
    }

    bool Interpreter::handleMessage(const std::string &inJson, std::string &outJson)
    {
        cJSON *root = cJSON_Parse(inJson.c_str());
        if (!root)
        {
            outJson = R"({"status":"error","message":"invalid JSON"})";
            ESP_LOGE(TAG, "JSON inválido");
            return false;
        }

        cJSON *type = cJSON_GetObjectItem(root, "type");
        cJSON *target = cJSON_GetObjectItem(root, "target");
        cJSON *action = cJSON_GetObjectItem(root, "action");
        cJSON *idField = cJSON_GetObjectItem(root, "id");
        cJSON *payload = cJSON_GetObjectItem(root, "payload"); // pode ser qualquer tipo/objeto

        const char *cType = cJSON_IsString(type) ? type->valuestring : nullptr;
        const char *cTarget = cJSON_IsString(target) ? target->valuestring : nullptr;
        const char *cAction = cJSON_IsString(action) ? action->valuestring : nullptr;

        if (!cType || !cTarget || !cAction)
        {
            outJson = buildErr(cTarget, cAction, idField, "missing fields: type/target/action");
            cJSON_Delete(root);
            ESP_LOGW(TAG, "campos faltando");
            return true;
        }

        // Roteamento
        Handler h = nullptr;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = handlers_.find(makeKey(cTarget, cAction));
            if (it != handlers_.end())
                h = it->second;
        }

        if (!h)
        {
            outJson = buildErr(cTarget, cAction, idField, "no handler registered");
            cJSON_Delete(root);
            ESP_LOGW(TAG, "sem handler p/ %s/%s", cTarget, cAction);
            return true;
        }

        // Executa handler
        cJSON *data = nullptr;
        try
        {
            data = h(payload); // pode retornar nullptr
        }
        catch (...)
        {
            outJson = buildErr(cTarget, cAction, idField, "handler exception");
            cJSON_Delete(root);
            ESP_LOGE(TAG, "exceção no handler");
            return true;
        }

        outJson = buildOk(cTarget, cAction, idField, "ok", data);
        cJSON_Delete(root);
        return true;
    }

} // namespace wetzelmesh
