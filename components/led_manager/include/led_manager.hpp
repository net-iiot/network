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
        static void set_uart_enabled(bool enabled); // Estado da UART (só para Node Borda)
        static void set_gateway_uart_connected(bool connected); // Estado da UART (só para Gateway)
        static bool get_gateway_uart_connected(); // Obtém status UART do Gateway
        
        // Para teste de token: mantém LED aceso por tempo determinado
        static void set_led_on_for_duration(uint32_t duration_ms);
        
        // A função original on_packet_received() foi substituída por blink(TrafficSource source)
    };

}
