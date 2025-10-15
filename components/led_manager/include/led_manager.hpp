#pragma once
#include <stdint.h>

namespace WetzelMesh
{
    // Novo enum para diferenciar a origem do tráfego para o LED de pisca
    enum class TrafficSource
    {
        MESH,  // ESPNOW / BLE
        UART,  // UART Border / Gateway
        SERVER // Comunicação com o servidor (apenas TX do Gateway)
    };

    class LedManager
    {
    public:
        static void init(bool isGateway);
        static void blink(TrafficSource source); // Novo: Recebe a fonte para piscar o LED correto

        static void set_node_joined(bool joined);
        static void set_gateway_server_connected(bool connected);
        static void set_uart_enabled(bool enabled); // Novo: Estado da UART (só para Node Borda)
        // A função original on_packet_received() foi substituída por blink(TrafficSource source)
    };

}
