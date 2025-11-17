#include "router.hpp"
#include "protocol.hpp"
#include "gateway.hpp"
#include "network_manager.hpp"
#include "led_manager.hpp"
#include "espnow_transport.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
        // Quando recebe dados da mesh, aguarda 1 segundo antes de repassar
        if (pkt.route.dst == "gateway" || pkt.route.dst == "broadcast" || 
            (pkt.type == Protocol::PacketType::EVENT && pkt.method == "DATA"))
        {
            // Node recebeu dado da mesh - mantém LED aceso durante 1 segundo antes de repassar
            ESP_LOGI(TAG, "Node recebeu dado da mesh - mantendo LED aceso por 1 segundo antes de repassar...");
            LedManager::set_led_on_for_duration(1000); // Mantém LED aceso por 1 segundo
            vTaskDelay(pdMS_TO_TICKS(1000)); // Aguarda 1 segundo
            
            // Envia pela malha; lógica "borda→UART" agora está em NetworkManager::send()
            ESP_LOGI(TAG, "Repassando dado após 1 segundo...");
            ESPNOWTransport::send(pkt);
            return;
        }

        // demais destinos específicos — aqui entraria roteamento por vizinho
    }

} // namespace WetzelMesh
