#pragma once
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
        std::string src; // origem (ex: "node-01")
        std::string dst; // destino (ex: "gateway", "flutter", "node-03")
    };

    struct Packet
    {
        PacketType type;      // tipo do pacote
        RouteInfo route;      // origem/destino
        std::string method;   // "GET"/"POST" (quando REQUEST)
        std::string endpoint; // ex: "/telemetry"
        int status = 0;       // status estilo HTTP (200, 404...) em RESPONSE
        std::string body;     // JSON (string)
    };

    // Constrói um Packet a partir de string JSON
    bool parse(const std::string &jsonStr, Packet &outPacket);

    // Serializa um Packet para string JSON
    std::string serialize(const Packet &packet);

    // Helpers
    Packet make_request(const std::string &src, const std::string &dst,
                        const std::string &method, const std::string &endpoint,
                        const std::string &body);

    Packet make_response(const std::string &src, const std::string &dst,
                         int status, const std::string &body);

} // namespace WetzelMesh::Protocol
