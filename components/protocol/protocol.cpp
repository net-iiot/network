#include "protocol.hpp"
#include "cJSON.h"
#include <sstream>
#include <random>
#include <iomanip>
#include <algorithm>
#include "esp_timer.h"
#include "esp_random.h"

namespace WetzelMesh::Protocol
{
    // Tamanho máximo de chunk (deixar espaço para headers)
    static constexpr size_t kMaxChunkSize = 1500;
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
        cJSON *jrequest_id = cJSON_GetObjectItem(root, "request_id");
        cJSON *jis_chunk = cJSON_GetObjectItem(root, "is_chunk");
        cJSON *jchunk_id = cJSON_GetObjectItem(root, "chunk_id");
        cJSON *jchunk_total = cJSON_GetObjectItem(root, "chunk_total");
        cJSON *jchunk_index = cJSON_GetObjectItem(root, "chunk_index");
        cJSON *jtopology = cJSON_GetObjectItem(root, "topology");

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
        if (jrequest_id && cJSON_IsString(jrequest_id))
            out.request_id = jrequest_id->valuestring;
        
        // Campos de chunk
        if (jis_chunk && cJSON_IsBool(jis_chunk))
            out.is_chunk = cJSON_IsTrue(jis_chunk);
        if (jchunk_id && cJSON_IsNumber(jchunk_id))
            out.chunk_id = jchunk_id->valuedouble;
        if (jchunk_total && cJSON_IsNumber(jchunk_total))
            out.chunk_total = jchunk_total->valuedouble;
        if (jchunk_index && cJSON_IsNumber(jchunk_index))
            out.chunk_index = jchunk_index->valuedouble;

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

        // Parse de topologia
        if (jtopology && cJSON_IsObject(jtopology))
        {
            cJSON *jnode_id = cJSON_GetObjectItem(jtopology, "node_id");
            cJSON *jneighbors = cJSON_GetObjectItem(jtopology, "neighbors");
            cJSON *jhas_gateway = cJSON_GetObjectItem(jtopology, "has_gateway");
            cJSON *jgateway_id = cJSON_GetObjectItem(jtopology, "gateway_id");

            if (jnode_id && cJSON_IsString(jnode_id))
                out.topology.node_id = jnode_id->valuestring;
            if (jhas_gateway && cJSON_IsBool(jhas_gateway))
                out.topology.has_gateway = cJSON_IsTrue(jhas_gateway);
            if (jgateway_id && cJSON_IsString(jgateway_id))
                out.topology.gateway_id = jgateway_id->valuestring;

            if (jneighbors && cJSON_IsArray(jneighbors))
            {
                int size = cJSON_GetArraySize(jneighbors);
                for (int i = 0; i < size; i++)
                {
                    cJSON *neighbor = cJSON_GetArrayItem(jneighbors, i);
                    if (neighbor && cJSON_IsObject(neighbor))
                    {
                        NeighborInfo nbr;
                        cJSON *jnid = cJSON_GetObjectItem(neighbor, "node_id");
                        cJSON *jrssi = cJSON_GetObjectItem(neighbor, "rssi");
                        cJSON *jlast_seen = cJSON_GetObjectItem(neighbor, "last_seen_ms");

                        if (jnid && cJSON_IsString(jnid))
                            nbr.node_id = jnid->valuestring;
                        if (jrssi && cJSON_IsNumber(jrssi))
                            nbr.rssi = jrssi->valuedouble;
                        if (jlast_seen && cJSON_IsNumber(jlast_seen))
                            nbr.last_seen_ms = jlast_seen->valuedouble;

                        out.topology.neighbors.push_back(nbr);
                    }
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
        if (!p.request_id.empty())
            cJSON_AddStringToObject(root, "request_id", p.request_id.c_str());

        // Campos de chunk
        if (p.is_chunk)
        {
            cJSON_AddBoolToObject(root, "is_chunk", true);
            cJSON_AddNumberToObject(root, "chunk_id", p.chunk_id);
            cJSON_AddNumberToObject(root, "chunk_total", p.chunk_total);
            cJSON_AddNumberToObject(root, "chunk_index", p.chunk_index);
        }

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

        // Serializar topologia
        if (!p.topology.node_id.empty() || !p.topology.neighbors.empty() || p.topology.has_gateway)
        {
            cJSON *topology = cJSON_CreateObject();
            if (!p.topology.node_id.empty())
                cJSON_AddStringToObject(topology, "node_id", p.topology.node_id.c_str());
            cJSON_AddBoolToObject(topology, "has_gateway", p.topology.has_gateway);
            if (!p.topology.gateway_id.empty())
                cJSON_AddStringToObject(topology, "gateway_id", p.topology.gateway_id.c_str());

            if (!p.topology.neighbors.empty())
            {
                cJSON *neighbors = cJSON_CreateArray();
                for (const auto &nbr : p.topology.neighbors)
                {
                    cJSON *neighbor = cJSON_CreateObject();
                    cJSON_AddStringToObject(neighbor, "node_id", nbr.node_id.c_str());
                    cJSON_AddNumberToObject(neighbor, "rssi", nbr.rssi);
                    cJSON_AddNumberToObject(neighbor, "last_seen_ms", nbr.last_seen_ms);
                    cJSON_AddItemToArray(neighbors, neighbor);
                }
                cJSON_AddItemToObject(topology, "neighbors", neighbors);
            }
            cJSON_AddItemToObject(root, "topology", topology);
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

    std::string generate_request_id()
    {
        // Gera ID único baseado em timestamp + random
        uint64_t timestamp = esp_timer_get_time() / 1000ULL; // ms
        uint32_t random_val = esp_random();
        
        std::ostringstream oss;
        oss << std::hex << timestamp << "-" << random_val;
        return oss.str();
    }

    std::vector<Packet> create_chunks(const Packet &original, size_t max_chunk_size)
    {
        std::vector<Packet> chunks;
        
        if (max_chunk_size == 0)
            max_chunk_size = kMaxChunkSize;

        // Serializa o pacote original para calcular tamanho
        std::string serialized = serialize(original);
        
        // Se o pacote já cabe em um chunk, retorna ele mesmo
        if (serialized.size() <= max_chunk_size)
        {
            chunks.push_back(original);
            return chunks;
        }

        // Divide o body em chunks
        size_t body_size = original.body.size();
        uint32_t chunk_id = esp_random(); // ID único para este conjunto de chunks
        size_t num_chunks = (body_size + max_chunk_size - 1) / max_chunk_size;

        for (size_t i = 0; i < num_chunks; i++)
        {
            Packet chunk = original;
            chunk.is_chunk = true;
            chunk.chunk_id = chunk_id;
            chunk.chunk_total = num_chunks;
            chunk.chunk_index = i;

            size_t start = i * max_chunk_size;
            size_t end = std::min(start + max_chunk_size, body_size);
            chunk.body = original.body.substr(start, end - start);

            chunks.push_back(chunk);
        }

        return chunks;
    }

    bool reconstruct_from_chunks(const std::vector<Packet> &chunks, Packet &out)
    {
        if (chunks.empty())
            return false;

        // Verifica se todos os chunks pertencem ao mesmo conjunto
        uint32_t chunk_id = chunks[0].chunk_id;
        uint32_t chunk_total = chunks[0].chunk_total;

        // Ordena por chunk_index
        std::vector<Packet> sorted_chunks = chunks;
        std::sort(sorted_chunks.begin(), sorted_chunks.end(),
                  [](const Packet &a, const Packet &b) {
                      return a.chunk_index < b.chunk_index;
                  });

        // Verifica se temos todos os chunks
        if (sorted_chunks.size() != chunk_total)
        {
            // Não temos todos os chunks, mas podemos tentar reconstruir com o que temos
            // (faltando alguns chunks)
        }

        // Reconstrói o pacote original a partir do primeiro chunk
        out = sorted_chunks[0];
        out.is_chunk = false;
        out.chunk_id = 0;
        out.chunk_total = 0;
        out.chunk_index = 0;

        // Reconstrói o body concatenando todos os chunks
        out.body.clear();
        for (const auto &chunk : sorted_chunks)
        {
            if (chunk.chunk_id != chunk_id)
                continue; // Ignora chunks de outro conjunto
            out.body += chunk.body;
        }

        return true;
    }

} // namespace WetzelMesh::Protocol
