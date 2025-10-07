#include "protocol.hpp"
#include "cJSON.h"

namespace Protocol
{

    static const char *typeToString(PacketType type)
    {
        switch (type)
        {
        case PacketType::REQUEST:
            return "request";
        case PacketType::RESPONSE:
            return "response";
        case PacketType::EVENT:
            return "event";
        default:
            return "unknown";
        }
    }

    static PacketType stringToType(const std::string &str)
    {
        if (str == "request")
            return PacketType::REQUEST;
        if (str == "response")
            return PacketType::RESPONSE;
        if (str == "event")
            return PacketType::EVENT;
        return PacketType::UNKNOWN;
    }

    bool parse(const std::string &jsonStr, Packet &outPacket)
    {
        cJSON *root = cJSON_Parse(jsonStr.c_str());
        if (!root)
            return false;

        cJSON *type = cJSON_GetObjectItem(root, "type");
        cJSON *route = cJSON_GetObjectItem(root, "route");
        cJSON *body = cJSON_GetObjectItem(root, "body");

        if (!type || !route)
        {
            cJSON_Delete(root);
            return false;
        }

        outPacket.type = stringToType(type->valuestring);
        outPacket.route.src = cJSON_GetObjectItem(route, "src")->valuestring;
        outPacket.route.dst = cJSON_GetObjectItem(route, "dst")->valuestring;

        cJSON *method = cJSON_GetObjectItem(root, "method");
        if (method)
            outPacket.method = method->valuestring;

        cJSON *endpoint = cJSON_GetObjectItem(root, "endpoint");
        if (endpoint)
            outPacket.endpoint = endpoint->valuestring;

        cJSON *status = cJSON_GetObjectItem(root, "status");
        if (status)
            outPacket.status = status->valueint;

        if (body)
            outPacket.body = cJSON_PrintUnformatted(body);
        else
            outPacket.body = "{}";

        cJSON_Delete(root);
        return true;
    }

    std::string serialize(const Packet &pkt)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", typeToString(pkt.type));

        cJSON *route = cJSON_CreateObject();
        cJSON_AddStringToObject(route, "src", pkt.route.src.c_str());
        cJSON_AddStringToObject(route, "dst", pkt.route.dst.c_str());
        cJSON_AddItemToObject(root, "route", route);

        if (pkt.type == PacketType::REQUEST)
        {
            cJSON_AddStringToObject(root, "method", pkt.method.c_str());
            cJSON_AddStringToObject(root, "endpoint", pkt.endpoint.c_str());
        }

        if (pkt.type == PacketType::RESPONSE)
        {
            cJSON_AddNumberToObject(root, "status", pkt.status);
        }

        cJSON *bodyJson = cJSON_Parse(pkt.body.c_str());
        if (bodyJson)
        {
            cJSON_AddItemToObject(root, "body", bodyJson);
        }
        else
        {
            cJSON_AddStringToObject(root, "body", pkt.body.c_str());
        }

        char *out = cJSON_PrintUnformatted(root);
        std::string result(out);
        cJSON_free(out);
        cJSON_Delete(root);

        return result;
    }

    Packet make_request(const std::string &src, const std::string &dst,
                        const std::string &method, const std::string &endpoint,
                        const std::string &body)
    {
        Packet pkt;
        pkt.type = PacketType::REQUEST;
        pkt.route = {src, dst};
        pkt.method = method;
        pkt.endpoint = endpoint;
        pkt.body = body;
        return pkt;
    }

    Packet make_response(const std::string &src, const std::string &dst,
                         int status, const std::string &body)
    {
        Packet pkt;
        pkt.type = PacketType::RESPONSE;
        pkt.route = {src, dst};
        pkt.status = status;
        pkt.body = body;
        return pkt;
    }

}
