#pragma once
#include "json_codec.hpp"
#include <string>

namespace WetzelMesh::Protocol
{

    enum class PacketType
    {
        REQUEST,
        RESPONSE,
        EVENT,
        UNKNOWN
    };

    struct RouteInfo
    {
        std::string src; // origem (ex: node-A, gateway)
        std::string dst; // destino (ex: gateway, server)
    };

    struct Packet
    {
        PacketType type;      // tipo de pacote
        RouteInfo route;      // origem/destino
        std::string method;   // "GET", "POST" (para request)
        std::string endpoint; // ex: "/api/data"
        int status;           // HTTP-like status (200, 404)
        std::string body;     // corpo JSON em string
    };

    // Constrói um pacote a partir de string JSON
    bool parse(const std::string &jsonStr, Packet &outPacket);

    // Serializa um pacote para string JSON
    std::string serialize(const Packet &pkt);

    // Helper: cria pacote de requisição
    Packet make_request(const std::string &src, const std::string &dst,
                        const std::string &method, const std::string &endpoint,
                        const std::string &body);

    // Helper: cria pacote de resposta
    Packet make_response(const std::string &src, const std::string &dst,
                         int status, const std::string &body);

}
