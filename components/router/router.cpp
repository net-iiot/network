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
        (void)isGateway;         // por ora não usamos
        ESPNOWTransport::init(); // inicia o transporte
    }

    void Router::handle_packet(const Protocol::Packet &pkt)
    {
        // log sem tamanho (evita usar campo que não existe)
        ESP_LOGI(TAG, "Roteando pacote: %s -> %s",
                 pkt.route.src.c_str(), pkt.route.dst.c_str());

        // pisca RX (esta existe no seu projeto)
        LedManager::on_packet_received();

        if (pkt.route.dst == "gateway")
        {
            Gateway::send(pkt);
            // LedManager::on_packet_sent(); // <- remover/ comentar: função não existe
            return;
        }

        NetworkManager::send(pkt);
        // LedManager::on_packet_sent();     // <- remover/ comentar
    }

} // namespace WetzelMesh
