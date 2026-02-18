#pragma once

#include <string>
#include "protocol.hpp"  // ✅ necessário para reconhecer Protocol::Packet

namespace WetzelMesh
{
    class Gateway
    {
    public:
        static void init(const std::string &server_url = "");
        static void uart_listen_task(void *);
        static bool send(const Protocol::Packet &pkt);
        static bool send_to_border(const Protocol::Packet &pkt);
        static bool uart_write_json(const std::string &json);
        
        // Cliente HTTP
        static bool send_http_request(const Protocol::Packet &request_pkt);
        static void set_server_url(const std::string &url);
        static std::string get_server_url();

        // Envia feedback de resultado OTA ao servidor
        static void send_ota_report(bool success, const std::string &command_id, const std::string &node_id, const std::string &error_msg);
        
    private:
        static std::string s_server_url;
        static void http_request_task(void *);
        static void polling_task(void *);  // Task unificada: map + OTA commands
    };
} // namespace WetzelMesh
