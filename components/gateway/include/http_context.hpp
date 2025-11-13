#pragma once
#include <string>
#include <map>
#include <memory>
#include "protocol.hpp"
#include "esp_timer.h"

namespace WetzelMesh
{
    // Contexto de uma requisição HTTP para manter referência de origem
    struct HttpRequestContext
    {
        std::string request_id;           // ID único da requisição
        std::string original_src;         // node-01 (origem original)
        std::string original_dst;          // server ou endpoint específico
        std::string method;                // GET, POST, etc.
        std::string endpoint;              // /api/telemetry
        std::string server_url;            // URL do servidor (extraída do endpoint ou config)
        uint64_t timestamp_ms;             // Quando foi criado
        bool is_completed = false;         // Se já foi completado
    };

    // Gerenciador de contexto HTTP
    class HttpContextManager
    {
    public:
        static HttpContextManager &instance();

        // Cria um novo contexto e retorna o request_id
        std::string create_context(const Protocol::Packet &request_pkt, const std::string &server_url = "");

        // Recupera contexto por request_id
        std::shared_ptr<HttpRequestContext> get_context(const std::string &request_id);

        // Remove contexto (após completar requisição)
        void remove_context(const std::string &request_id);

        // Limpa contextos antigos (timeout)
        void cleanup_old_contexts(uint64_t timeout_ms = 300000); // 5 minutos default

    private:
        HttpContextManager() = default;
        std::map<std::string, std::shared_ptr<HttpRequestContext>> contexts_;
    };

} // namespace WetzelMesh

