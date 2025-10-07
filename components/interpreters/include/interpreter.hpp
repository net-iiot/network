#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include "cJSON.h"

namespace wetzelmesh
{

    /**
     * Protocolo WetzelMesh (camada de mensageria):
     * JSON esperado (exemplo):
     * {
     *   "type": "command",        // "command" | "event" | "query"
     *   "target": "led",          // recurso/serviço
     *   "action": "on",           // operação
     *   "from": "node_01",        // opcional
     *   "id": 123,                // opcional: correlação
     *   "payload": { ... }        // opcional: dados específicos
     * }
     *
     * Resposta padrão:
     * {
     *   "status": "ok" | "error",
     *   "target": "<igual ao pedido>",
     *   "action": "<igual ao pedido>",
     *   "id": 123,                // se veio no pedido
     *   "message": "texto",
     *   "data": { ... }           // opcional
     * }
     */
    class Interpreter
    {
    public:
        // Handler recebe o objeto "payload" (pode ser null) e retorna um cJSON* com o conteúdo de "data" (ou nullptr).
        using Handler = std::function<cJSON *(const cJSON *payload)>;

        // Registra manipulador para par <target, action>
        void registerHandler(const std::string &target, const std::string &action, Handler h);

        // Processa uma mensagem JSON e retorna a resposta JSON (string). true = sucesso de parsing/roteamento (mesmo que status="error").
        bool handleMessage(const std::string &inJson, std::string &outJson);

    private:
        std::string makeKey(const std::string &target, const std::string &action) const;

        // Helpers de resposta
        static std::string buildOk(const char *target, const char *action, const cJSON *idField, const char *message, cJSON *data /*ownership transfer*/);
        static std::string buildErr(const char *target, const char *action, const cJSON *idField, const char *message);

        std::map<std::string, Handler> handlers_;
        std::mutex mtx_;
    };

} // namespace wetzelmesh
