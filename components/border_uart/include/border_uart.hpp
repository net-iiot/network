#pragma once
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{

    class BorderUart
    {
    public:
        using RxHandler = void (*)(const Protocol::Packet &pkt);

        // Inicializa a UART do nó-borda (idempotente). Seguro rodar em qualquer nó:
        // só "ativa" de fato se o hardware estiver cabeado.
        static bool init();

        // Envia pacote ao GATEWAY via UART (nó-borda → gateway).
        static bool send_to_gateway(const Protocol::Packet &pkt);

        // Informa se a UART está ativa.
        static bool is_enabled();

        // Registra o callback para pacotes recebidos do GATEWAY (gateway → borda).
        static void set_rx_handler(RxHandler cb);

    private:
        static bool uart_write_json(const std::string &json);
        static void uart_listen_task(void *); // lê do gateway e aciona o handler
    };

} // namespace WetzelMesh
