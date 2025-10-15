#pragma once
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{

    class Gateway
    {
    public:
        // Inicializa o gateway: BLE/BT off e UART ativa (GW <-> Borda)
        static void init();

        // Envia pacote para o "servidor" (stub - para futuros HTTP/MQTT, etc)
        static bool send(const Protocol::Packet &pkt);

        // Envia pela UART ao nó-borda (mesmo board)
        static bool send_to_border(const Protocol::Packet &pkt);

        // Task de RX UART (GW <- Borda)
        static void uart_listen_task(void *);

    private:
        // Escrita de JSON com framing "<len>\n<json>" na UART
        static bool uart_write_json(const std::string &json);
    };

} // namespace WetzelMesh
