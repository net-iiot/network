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
        cJSON *jtrace = cJSON_GetObjectItem(root, "trace");
        cJSON *jnext_hop = cJSON_GetObjectItem(root, "next_hop");
        cJSON *jrouting_strategy = cJSON_GetObjectItem(root, "routing_strategy");
        cJSON *jttl = cJSON_GetObjectItem(root, "ttl");

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

        // Parse de metadados de roteamento
        if (jnext_hop && cJSON_IsString(jnext_hop))
            out.next_hop = jnext_hop->valuestring;
        if (jrouting_strategy && cJSON_IsString(jrouting_strategy))
            out.routing_strategy = jrouting_strategy->valuestring;
        if (jttl && cJSON_IsNumber(jttl))
            out.ttl = jttl->valuedouble;

        // Parse de trace
        if (jtrace && cJSON_IsObject(jtrace))
        {
            cJSON *jpath = cJSON_GetObjectItem(jtrace, "path");
            cJSON *jhop_count = cJSON_GetObjectItem(jtrace, "hop_count");
            cJSON *jcreated_at = cJSON_GetObjectItem(jtrace, "created_at_ms");
            cJSON *jreceived_at = cJSON_GetObjectItem(jtrace, "received_at_ms");
            cJSON *jpacket_id = cJSON_GetObjectItem(jtrace, "packet_id");
            cJSON *jhop_history = cJSON_GetObjectItem(jtrace, "hop_history");

            if (jpath && cJSON_IsArray(jpath))
            {
                int size = cJSON_GetArraySize(jpath);
                for (int i = 0; i < size; i++)
                {
                    cJSON *item = cJSON_GetArrayItem(jpath, i);
                    if (item && cJSON_IsString(item))
                        out.trace.path.push_back(item->valuestring);
                }
            }
            if (jhop_count && cJSON_IsNumber(jhop_count))
                out.trace.hop_count = jhop_count->valuedouble;
            if (jcreated_at && cJSON_IsNumber(jcreated_at))
                out.trace.created_at_ms = jcreated_at->valuedouble;
            if (jreceived_at && cJSON_IsNumber(jreceived_at))
                out.trace.received_at_ms = jreceived_at->valuedouble;
            if (jpacket_id && cJSON_IsString(jpacket_id))
                out.trace.packet_id = jpacket_id->valuestring;

            if (jhop_history && cJSON_IsArray(jhop_history))
            {
                int size = cJSON_GetArraySize(jhop_history);
                for (int i = 0; i < size; i++)
                {
                    cJSON *hop = cJSON_GetArrayItem(jhop_history, i);
                    if (hop && cJSON_IsObject(hop))
                    {
                        HopInfo hop_info;
                        cJSON *jnode_id = cJSON_GetObjectItem(hop, "node_id");
                        cJSON *jnode_name = cJSON_GetObjectItem(hop, "node_name");
                        cJSON *jtimestamp = cJSON_GetObjectItem(hop, "timestamp_ms");
                        cJSON *jrssi = cJSON_GetObjectItem(hop, "rssi");
                        cJSON *jtransport = cJSON_GetObjectItem(hop, "transport");

                        if (jnode_id && cJSON_IsString(jnode_id))
                            hop_info.node_id = jnode_id->valuestring;
                        if (jnode_name && cJSON_IsString(jnode_name))
                            hop_info.node_name = jnode_name->valuestring;
                        if (jtimestamp && cJSON_IsNumber(jtimestamp))
                            hop_info.timestamp_ms = jtimestamp->valuedouble;
                        if (jrssi && cJSON_IsNumber(jrssi))
                            hop_info.rssi = jrssi->valuedouble;
                        if (jtransport && cJSON_IsString(jtransport))
                            hop_info.transport = jtransport->valuestring;

                        out.trace.hop_history.push_back(hop_info);
                    }
                }
            }
        }

        // Parse de topologia
        if (jtopology && cJSON_IsObject(jtopology))
        {
            cJSON *jnode_id = cJSON_GetObjectItem(jtopology, "node_id");
            cJSON *jnode_name = cJSON_GetObjectItem(jtopology, "node_name");
            cJSON *jnetwork_id = cJSON_GetObjectItem(jtopology, "network_id");
            cJSON *jneighbors = cJSON_GetObjectItem(jtopology, "neighbors");
            cJSON *jhas_gateway = cJSON_GetObjectItem(jtopology, "has_gateway");
            cJSON *jgateway_id = cJSON_GetObjectItem(jtopology, "gateway_id");
            cJSON *jconnectivity = cJSON_GetObjectItem(jtopology, "connectivity");
            cJSON *jnode_info = cJSON_GetObjectItem(jtopology, "node_info");

            if (jnode_id && cJSON_IsString(jnode_id))
                out.topology.node_id = jnode_id->valuestring;
            if (jnode_name && cJSON_IsString(jnode_name))
                out.topology.node_name = jnode_name->valuestring;
            if (jnetwork_id && cJSON_IsString(jnetwork_id))
                out.topology.network_id = jnetwork_id->valuestring;
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

            // Parse de connectivity matrix
            if (jconnectivity && cJSON_IsObject(jconnectivity))
            {
                cJSON *jconnections = cJSON_GetObjectItem(jconnectivity, "connections");
                if (jconnections && cJSON_IsArray(jconnections))
                {
                    int size = cJSON_GetArraySize(jconnections);
                    for (int i = 0; i < size; i++)
                    {
                        cJSON *conn = cJSON_GetArrayItem(jconnections, i);
                        if (conn && cJSON_IsObject(conn))
                        {
                            Connection connection;
                            cJSON *jfrom = cJSON_GetObjectItem(conn, "from_node_id");
                            cJSON *jto = cJSON_GetObjectItem(conn, "to_node_id");
                            cJSON *jrssi = cJSON_GetObjectItem(conn, "rssi");
                            cJSON *jlast_comm = cJSON_GetObjectItem(conn, "last_communication_ms");
                            cJSON *jpacket_count = cJSON_GetObjectItem(conn, "packet_count");
                            cJSON *jis_direct = cJSON_GetObjectItem(conn, "is_direct");

                            if (jfrom && cJSON_IsString(jfrom))
                                connection.from_node_id = jfrom->valuestring;
                            if (jto && cJSON_IsString(jto))
                                connection.to_node_id = jto->valuestring;
                            if (jrssi && cJSON_IsNumber(jrssi))
                                connection.rssi = jrssi->valuedouble;
                            if (jlast_comm && cJSON_IsNumber(jlast_comm))
                                connection.last_communication_ms = jlast_comm->valuedouble;
                            if (jpacket_count && cJSON_IsNumber(jpacket_count))
                                connection.packet_count = jpacket_count->valuedouble;
                            if (jis_direct && cJSON_IsBool(jis_direct))
                                connection.is_direct = cJSON_IsTrue(jis_direct);

                            out.topology.connectivity.connections.push_back(connection);
                        }
                    }
                }
            }

            // Parse de node_info
            if (jnode_info && cJSON_IsObject(jnode_info))
            {
                cJSON *jnode_id = cJSON_GetObjectItem(jnode_info, "node_id");
                cJSON *jnode_name = cJSON_GetObjectItem(jnode_info, "node_name");
                cJSON *jnode_type = cJSON_GetObjectItem(jnode_info, "node_type");
                cJSON *jcapabilities = cJSON_GetObjectItem(jnode_info, "capabilities");
                cJSON *jfirst_seen = cJSON_GetObjectItem(jnode_info, "first_seen_ms");
                cJSON *jlast_seen = cJSON_GetObjectItem(jnode_info, "last_seen_ms");
                cJSON *jbattery = cJSON_GetObjectItem(jnode_info, "battery_level");
                cJSON *jpos_x = cJSON_GetObjectItem(jnode_info, "position_x");
                cJSON *jpos_y = cJSON_GetObjectItem(jnode_info, "position_y");
                cJSON *jmetadata = cJSON_GetObjectItem(jnode_info, "metadata");

                if (jnode_id && cJSON_IsString(jnode_id))
                    out.topology.node_info.node_id = jnode_id->valuestring;
                if (jnode_name && cJSON_IsString(jnode_name))
                    out.topology.node_info.node_name = jnode_name->valuestring;
                if (jnode_type && cJSON_IsString(jnode_type))
                    out.topology.node_info.node_type = jnode_type->valuestring;
                if (jfirst_seen && cJSON_IsNumber(jfirst_seen))
                    out.topology.node_info.first_seen_ms = jfirst_seen->valuedouble;
                if (jlast_seen && cJSON_IsNumber(jlast_seen))
                    out.topology.node_info.last_seen_ms = jlast_seen->valuedouble;
                if (jbattery && cJSON_IsNumber(jbattery))
                    out.topology.node_info.battery_level = jbattery->valuedouble;
                if (jpos_x && cJSON_IsNumber(jpos_x))
                    out.topology.node_info.position_x = jpos_x->valuedouble;
                if (jpos_y && cJSON_IsNumber(jpos_y))
                    out.topology.node_info.position_y = jpos_y->valuedouble;

                if (jcapabilities && cJSON_IsArray(jcapabilities))
                {
                    int size = cJSON_GetArraySize(jcapabilities);
                    for (int i = 0; i < size; i++)
                    {
                        cJSON *cap = cJSON_GetArrayItem(jcapabilities, i);
                        if (cap && cJSON_IsString(cap))
                            out.topology.node_info.capabilities.push_back(cap->valuestring);
                    }
                }

                if (jmetadata && cJSON_IsObject(jmetadata))
                {
                    cJSON *item = jmetadata->child;
                    while (item)
                    {
                        if (cJSON_IsString(item))
                            out.topology.node_info.metadata[item->string] = item->valuestring;
                        item = item->next;
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

        // Serializar metadados de roteamento
        if (!p.next_hop.empty())
            cJSON_AddStringToObject(root, "next_hop", p.next_hop.c_str());
        if (!p.routing_strategy.empty())
            cJSON_AddStringToObject(root, "routing_strategy", p.routing_strategy.c_str());
        if (p.ttl != 10) // Só adiciona se diferente do padrão
            cJSON_AddNumberToObject(root, "ttl", p.ttl);

        // Serializar trace
        if (!p.trace.path.empty() || p.trace.hop_count > 0 || p.trace.created_at_ms > 0 || !p.trace.packet_id.empty())
        {
            cJSON *trace = cJSON_CreateObject();
            
            if (!p.trace.path.empty())
            {
                cJSON *path = cJSON_CreateArray();
                for (const auto &node : p.trace.path)
                {
                    cJSON_AddItemToArray(path, cJSON_CreateString(node.c_str()));
                }
                cJSON_AddItemToObject(trace, "path", path);
            }
            
            if (p.trace.hop_count > 0)
                cJSON_AddNumberToObject(trace, "hop_count", p.trace.hop_count);
            if (p.trace.created_at_ms > 0)
                cJSON_AddNumberToObject(trace, "created_at_ms", p.trace.created_at_ms);
            if (p.trace.received_at_ms > 0)
                cJSON_AddNumberToObject(trace, "received_at_ms", p.trace.received_at_ms);
            if (!p.trace.packet_id.empty())
                cJSON_AddStringToObject(trace, "packet_id", p.trace.packet_id.c_str());

            if (!p.trace.hop_history.empty())
            {
                cJSON *hop_history = cJSON_CreateArray();
                for (const auto &hop : p.trace.hop_history)
                {
                    cJSON *hop_obj = cJSON_CreateObject();
                    cJSON_AddStringToObject(hop_obj, "node_id", hop.node_id.c_str());
                    if (!hop.node_name.empty())
                        cJSON_AddStringToObject(hop_obj, "node_name", hop.node_name.c_str());
                    cJSON_AddNumberToObject(hop_obj, "timestamp_ms", hop.timestamp_ms);
                    cJSON_AddNumberToObject(hop_obj, "rssi", hop.rssi);
                    if (!hop.transport.empty())
                        cJSON_AddStringToObject(hop_obj, "transport", hop.transport.c_str());
                    cJSON_AddItemToArray(hop_history, hop_obj);
                }
                cJSON_AddItemToObject(trace, "hop_history", hop_history);
            }
            
            cJSON_AddItemToObject(root, "trace", trace);
        }

        // Serializar topologia
        if (!p.topology.node_id.empty() || !p.topology.neighbors.empty() || p.topology.has_gateway || 
            !p.topology.node_name.empty() || !p.topology.network_id.empty() || 
            !p.topology.connectivity.connections.empty() || !p.topology.node_info.node_id.empty())
        {
            cJSON *topology = cJSON_CreateObject();
            if (!p.topology.node_id.empty())
                cJSON_AddStringToObject(topology, "node_id", p.topology.node_id.c_str());
            if (!p.topology.node_name.empty())
                cJSON_AddStringToObject(topology, "node_name", p.topology.node_name.c_str());
            if (!p.topology.network_id.empty())
                cJSON_AddStringToObject(topology, "network_id", p.topology.network_id.c_str());
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

            // Serializar connectivity matrix
            if (!p.topology.connectivity.connections.empty())
            {
                cJSON *connectivity = cJSON_CreateObject();
                cJSON *connections = cJSON_CreateArray();
                for (const auto &conn : p.topology.connectivity.connections)
                {
                    cJSON *conn_obj = cJSON_CreateObject();
                    cJSON_AddStringToObject(conn_obj, "from_node_id", conn.from_node_id.c_str());
                    cJSON_AddStringToObject(conn_obj, "to_node_id", conn.to_node_id.c_str());
                    cJSON_AddNumberToObject(conn_obj, "rssi", conn.rssi);
                    cJSON_AddNumberToObject(conn_obj, "last_communication_ms", conn.last_communication_ms);
                    cJSON_AddNumberToObject(conn_obj, "packet_count", conn.packet_count);
                    cJSON_AddBoolToObject(conn_obj, "is_direct", conn.is_direct);
                    cJSON_AddItemToArray(connections, conn_obj);
                }
                cJSON_AddItemToObject(connectivity, "connections", connections);
                cJSON_AddItemToObject(topology, "connectivity", connectivity);
            }

            // Serializar node_info
            if (!p.topology.node_info.node_id.empty())
            {
                cJSON *node_info = cJSON_CreateObject();
                cJSON_AddStringToObject(node_info, "node_id", p.topology.node_info.node_id.c_str());
                if (!p.topology.node_info.node_name.empty())
                    cJSON_AddStringToObject(node_info, "node_name", p.topology.node_info.node_name.c_str());
                if (!p.topology.node_info.node_type.empty())
                    cJSON_AddStringToObject(node_info, "node_type", p.topology.node_info.node_type.c_str());
                if (p.topology.node_info.first_seen_ms > 0)
                    cJSON_AddNumberToObject(node_info, "first_seen_ms", p.topology.node_info.first_seen_ms);
                if (p.topology.node_info.last_seen_ms > 0)
                    cJSON_AddNumberToObject(node_info, "last_seen_ms", p.topology.node_info.last_seen_ms);
                if (p.topology.node_info.battery_level >= 0)
                    cJSON_AddNumberToObject(node_info, "battery_level", p.topology.node_info.battery_level);
                if (p.topology.node_info.position_x != 0.0f || p.topology.node_info.position_y != 0.0f)
                {
                    cJSON_AddNumberToObject(node_info, "position_x", p.topology.node_info.position_x);
                    cJSON_AddNumberToObject(node_info, "position_y", p.topology.node_info.position_y);
                }

                if (!p.topology.node_info.capabilities.empty())
                {
                    cJSON *capabilities = cJSON_CreateArray();
                    for (const auto &cap : p.topology.node_info.capabilities)
                    {
                        cJSON_AddItemToArray(capabilities, cJSON_CreateString(cap.c_str()));
                    }
                    cJSON_AddItemToObject(node_info, "capabilities", capabilities);
                }

                if (!p.topology.node_info.metadata.empty())
                {
                    cJSON *metadata = cJSON_CreateObject();
                    for (const auto &pair : p.topology.node_info.metadata)
                    {
                        cJSON_AddStringToObject(metadata, pair.first.c_str(), pair.second.c_str());
                    }
                    cJSON_AddItemToObject(node_info, "metadata", metadata);
                }

                cJSON_AddItemToObject(topology, "node_info", node_info);
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

    std::string generate_packet_id()
    {
        // Gera ID único estilo UUID para rastreamento de pacotes
        uint64_t timestamp = esp_timer_get_time() / 1000ULL; // ms
        uint32_t random1 = esp_random();
        uint32_t random2 = esp_random();
        uint32_t random3 = esp_random();
        
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(8) << (timestamp & 0xFFFFFFFF)
            << "-" << std::setw(4) << ((timestamp >> 32) & 0xFFFF)
            << "-" << std::setw(4) << (random1 & 0xFFFF)
            << "-" << std::setw(4) << ((random1 >> 16) & 0xFFFF)
            << "-" << std::setw(8) << (random2 & 0xFFFFFFFF)
            << std::setw(4) << (random3 & 0xFFFF);
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
