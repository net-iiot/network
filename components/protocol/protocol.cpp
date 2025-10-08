#include "protocol.hpp"
#include "cJSON.h"

namespace WetzelMesh::Protocol
{
    static const char *typeToString(PacketType type)
    {
        switch (type)
        {
        case PacketType::REQUEST:
            return "REQUEST";
        case PacketType::RESPONSE:
            return "RESPONSE";
        case PacketType::EVENT:
            return "EVENT";
        default:
            return "UNKNOWN";
        }
    }

    static PacketType stringToType(const std::string &str)
    {
        if (str == "REQUEST")
            return PacketType::REQUEST;
        if (str == "RESPONSE")
            return PacketType::RESPONSE;
        if (str == "EVENT")
            return PacketType::EVENT;
        return PacketType::UNKNOWN;
    }

    bool parse(const std::string &jsonStr, Packet &out)
    {
        cJSON *root = cJSON_Parse(jsonStr.c_str());
        if (!root)
            return false;

        auto cleanup = [&]()
        { cJSON_Delete(root); };

        cJSON *jtype = cJSON_GetObjectItem(root, "type");
        cJSON *jsrc = cJSON_GetObjectItem(root, "src");
        cJSON *jdst = cJSON_GetObjectItem(root, "dst");
        cJSON *jmethod = cJSON_GetObjectItem(root, "method");
        cJSON *jendpoint = cJSON_GetObjectItem(root, "endpoint");
        cJSON *jstatus = cJSON_GetObjectItem(root, "status");
        cJSON *jbody = cJSON_GetObjectItem(root, "body");

        if (!cJSON_IsString(jtype) || !cJSON_IsString(jsrc) || !cJSON_IsString(jdst))
        {
            cleanup();
            return false;
        }

        out.type = stringToType(jtype->valuestring);
        out.route.src = jsrc->valuestring;
        out.route.dst = jdst->valuestring;

        if (jmethod && cJSON_IsString(jmethod))
            out.method = jmethod->valuestring;
        if (jendpoint && cJSON_IsString(jendpoint))
            out.endpoint = jendpoint->valuestring;
        if (jstatus && cJSON_IsNumber(jstatus))
            out.status = jstatus->valuedouble;

        if (jbody)
        {
            if (cJSON_IsString(jbody))
                out.body = jbody->valuestring;
            else
            {
                // Se "body" vier como objeto/array, embala como string compacta
                char *printed = cJSON_PrintUnformatted(jbody);
                if (printed)
                {
                    out.body = printed;
                    cJSON_free(printed);
                }
            }
        }

        cleanup();
        return true;
    }

    std::string serialize(const Packet &p)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", typeToString(p.type));
        cJSON_AddStringToObject(root, "src", p.route.src.c_str());
        cJSON_AddStringToObject(root, "dst", p.route.dst.c_str());
        if (!p.method.empty())
            cJSON_AddStringToObject(root, "method", p.method.c_str());
        if (!p.endpoint.empty())
            cJSON_AddStringToObject(root, "endpoint", p.endpoint.c_str());
        if (p.status != 0)
            cJSON_AddNumberToObject(root, "status", p.status);

        // body pode ser string JSON válida — tentamos parsear; se falhar, vai como string
        cJSON *parsed = cJSON_Parse(p.body.c_str());
        if (parsed)
        {
            cJSON_AddItemToObject(root, "body", parsed);
        }
        else
        {
            cJSON_AddStringToObject(root, "body", p.body.c_str());
        }

        char *printed = cJSON_PrintUnformatted(root);
        std::string out = printed ? printed : "{}";
        if (printed)
            cJSON_free(printed);
        cJSON_Delete(root);
        return out;
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

} // namespace WetzelMesh::Protocol
