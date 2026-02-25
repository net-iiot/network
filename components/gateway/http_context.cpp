#include "http_context.hpp"
#include "esp_log.h"
#include <algorithm>

namespace NetworkMesh
{
    static const char *TAG = "HTTP_CTX";

    HttpContextManager &HttpContextManager::instance()
    {
        static HttpContextManager inst;
        return inst;
    }

    std::string HttpContextManager::create_context(const Protocol::Packet &request_pkt, const std::string &server_url)
    {
        auto ctx = std::make_shared<HttpRequestContext>();

        if (request_pkt.request_id.empty())
        {
            ctx->request_id = Protocol::generate_request_id();
        }
        else
        {
            ctx->request_id = request_pkt.request_id;
        }

        ctx->original_src = request_pkt.route.src;
        ctx->original_dst = request_pkt.route.dst;
        ctx->method = request_pkt.method;
        ctx->endpoint = request_pkt.endpoint;
        ctx->server_url = server_url.empty() ? "http://localhost" : server_url;
        ctx->timestamp_ms = esp_timer_get_time() / 1000ULL;
        ctx->is_completed = false;

        contexts_[ctx->request_id] = ctx;

        ESP_LOGI(TAG, "Contexto criado: req_id=%s src=%s endpoint=%s",
                 ctx->request_id.c_str(), ctx->original_src.c_str(), ctx->endpoint.c_str());

        return ctx->request_id;
    }

    std::shared_ptr<HttpRequestContext> HttpContextManager::get_context(const std::string &request_id)
    {
        auto it = contexts_.find(request_id);
        if (it != contexts_.end())
        {
            return it->second;
        }
        return nullptr;
    }

    void HttpContextManager::remove_context(const std::string &request_id)
    {
        contexts_.erase(request_id);
        ESP_LOGI(TAG, "Contexto removido: req_id=%s", request_id.c_str());
    }

    void HttpContextManager::cleanup_old_contexts(uint64_t timeout_ms)
    {
        uint64_t now = esp_timer_get_time() / 1000ULL;

        for (auto it = contexts_.begin(); it != contexts_.end();)
        {
            if ((now - it->second->timestamp_ms) > timeout_ms)
            {
                ESP_LOGW(TAG, "Removendo contexto expirado: req_id=%s", it->first.c_str());
                it = contexts_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}
