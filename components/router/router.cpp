#include "router.hpp"
#include "protocol.hpp"
#include "gateway.hpp"
#include "network_manager.hpp"
#include "led_manager.hpp"
#include "espnow_transport.hpp"
#include "esp_log.h"

namespace WetzelMesh
{

    static const char *TAG = "ROUTER";

    void Router::init(bool isGateway)
    {
        (void)isGateway;
        // transportes são inicializados no NetworkManager
    }

    void Router::handle_packet(const Protocol::Packet &pkt)
    {
        ESP_LOGI(TAG, "Roteando pacote: %s -> %s",
                 pkt.route.src.c_str(), pkt.route.dst.c_str());

        // Não roteia HELLO
        if (pkt.type == Protocol::PacketType::EVENT && pkt.method == std::string("HELLO"))
            return;

        if (NetworkManager::is_gateway())
        {
            // Gateway: servidor <-> UART borda
            if (pkt.route.dst == "server")
                Gateway::send(pkt);
            else
                Gateway::send_to_border(pkt);
            return;
        }

        // Em nó: roteamento básico
        if (pkt.route.dst == "gateway" || pkt.route.dst == "broadcast")
        {
            // Envia pela malha; lógica “borda→UART” agora está em NetworkManager::send()
            ESPNOWTransport::send(pkt);
            return;
        }

        // demais destinos específicos — aqui entraria roteamento por vizinho
    }

} // namespace WetzelMesh
