#pragma once
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{
    // Transporte BLE de alto nível (GATT: 1 serviço, RX/TX)
    class BLETransport
    {
    public:
        static void init(bool isGateway);
        static bool send(const Protocol::Packet &packet);           // envia JSON pelo TX notify
        static void on_receive_json(const std::string &jsonString); // callback interno -> Router

        // Opcional: obter nome do nó para src
        static std::string node_id();

    private:
        static void start_gap_gatt();
        static void start_advertising();
        static void setup_service();
        static void notify_tx(const std::string &data);

        static bool s_isGateway;
    };
} // namespace WetzelMesh
