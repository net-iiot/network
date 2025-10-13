#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include "protocol.hpp"

namespace WetzelMesh
{

    struct Neighbor
    {
        std::string id;        // ex: "node-ABCD"
        int rssi;              // RSSI (placeholder, preenche -40 se não tiver)
        uint64_t last_seen_ms; // timestamp do último HELLO
    };

    class NetworkManager
    {
    public:
        static void init(bool isGateway);
        static bool is_gateway();
        static const std::vector<Neighbor> &neighbors();

        // Envia um pacote pela malha (ESPNOW)
        static bool send(const Protocol::Packet &p);

        // Entrada comum de pacotes vindos de transportes
        static void handle_incoming(const Protocol::Packet &packet);

        // ===== NOVOS =====
        static void start_hello_task();                             // task periódica que envia HELLO
        static void on_hello(const std::string &node_id, int rssi); // atualiza a tabela de vizinhos
        static uint64_t now_ms();                                   // time utilitário

    private:
        static void refresh_neighbors_task(void *param);

        static bool s_gateway;
        static std::vector<Neighbor> s_neighbors;
    };

} // namespace WetzelMesh
