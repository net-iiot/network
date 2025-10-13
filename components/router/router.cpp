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
        // ESPNOW agora é inicializado no NetworkManager::init()
    }

    void Router::handle_packet(const Protocol::Packet &pkt)
    {
        ESP_LOGI(TAG, "Roteando pacote: %s -> %s",
                 pkt.route.src.c_str(), pkt.route.dst.c_str());

        LedManager::on_packet_received();

        // 1) Descoberta automática: EVENT/HELLO atualiza vizinhos e não reencaminha
        if (pkt.type == Protocol::PacketType::EVENT && pkt.method == std::string("HELLO"))
        {
            NetworkManager::on_hello(pkt.route.src, /*rssi=*/-40); // preenche -40 por enquanto
            return;
        }

        // 2) Destino "gateway" → envia pela ponte (UART/HTTP)
        if (pkt.route.dst == "gateway")
        {
            Gateway::send(pkt);
            return;
        }

        // 3) Default: se quiser manter broadcast, use NetworkManager::send(pkt) aqui.
        //    (Por padrão, não refaz broadcast para evitar looping.)
    }

} // namespace WetzelMesh
