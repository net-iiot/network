#pragma once
#include <string>
#include <map>
#include <memory>
#include "protocol.hpp"
#include "esp_timer.h"

namespace NetworkMesh
{
    struct HttpRequestContext
    {
        std::string request_id;
        std::string original_src;
        std::string original_dst;
        std::string method;
        std::string endpoint;
        std::string server_url;
        uint64_t timestamp_ms;
        bool is_completed = false;
    };

    class HttpContextManager
    {
    public:
        static HttpContextManager &instance();

        std::string create_context(const Protocol::Packet &request_pkt, const std::string &server_url = "");
        std::shared_ptr<HttpRequestContext> get_context(const std::string &request_id);
        void remove_context(const std::string &request_id);
        void cleanup_old_contexts(uint64_t timeout_ms = 300000);

    private:
        HttpContextManager() = default;
        std::map<std::string, std::shared_ptr<HttpRequestContext>> contexts_;
    };
}
