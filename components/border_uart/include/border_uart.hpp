#pragma once
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{

    class BorderUart
    {
    public:
        using RxHandler = void (*)(const Protocol::Packet &pkt);

        // Inicializa a UART do nó-borda.
        // Faz HANDSHAKE com o gateway (PING/PONG). Se falhar, desativa e retorna false.
        static bool init();

        // Envia pacote ao GATEWAY via UART (nó-borda → gateway).
        // Retorna false se a UART não estiver habilitada (sem cabo/handshake).
        static bool send_to_gateway(const Protocol::Packet &pkt);

        // Indica se a UART está ativa (handshake OK).
        static bool is_enabled();

        // Registra callback para pacotes recebidos do GATEWAY.
        static void set_rx_handler(RxHandler cb);

    private:
        static bool uart_write_json(const std::string &json);
        static void uart_listen_task(void *);          // lê do gateway e aciona o handler
        static bool do_handshake(unsigned timeout_ms); // PING → espera PONG
    };

} // namespace WetzelMesh
