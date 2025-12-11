#include "router.hpp"
#include "protocol.hpp"
#include "gateway.hpp"
#include "network_manager.hpp"
#include "led_manager.hpp"
#include "espnow_transport.hpp"
#include "ble_transport.hpp"
#include "border_uart.hpp"
#include "esp_timer.h"
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
        // IMPORTANTE: Se destino é "gateway" e este node tem UART habilitado, envia via UART
        if (pkt.route.dst == "gateway")
        {
            // Se este node tem UART (border node), envia direto para gateway via UART
            if (BorderUart::is_enabled())
            {
                ESP_LOGI(TAG, "Router: Pacote para gateway, enviando via UART (border node)");
                BorderUart::send_to_gateway(pkt);
                return;
            }
            // Se não tem UART, reencaminha pela mesh (continua o fluxo abaixo)
            ESP_LOGI(TAG, "Router: Pacote para gateway, reencaminhando pela mesh (node comum)...");
        }
        
        // Quando recebe dados da mesh, aguarda 1 segundo antes de repassar
        // IMPORTANTE: DISCOVERY (REQUEST) também precisa ser reencaminhado para flooding
        if (pkt.route.dst == "gateway" || pkt.route.dst == "broadcast" || 
            (pkt.type == Protocol::PacketType::EVENT && pkt.method == "DATA") ||
            (pkt.type == Protocol::PacketType::REQUEST && pkt.method == "DISCOVERY"))
        {
            // Atualizar informações de rastreabilidade
            Protocol::Packet updated_pkt = pkt; // Cópia para modificar
            std::string current_node_id = BLETransport::node_id();
            uint64_t now_ms = esp_timer_get_time() / 1000ULL;
            
            // Verificar se este node já está no path (detecção de loop)
            bool already_in_path = false;
            for (const auto &node : updated_pkt.trace.path)
            {
                if (node == current_node_id)
                {
                    already_in_path = true;
                    break;
                }
            }
            
            // Se já passou por este node, descarta imediatamente (loop detectado)
            if (already_in_path)
            {
                ESP_LOGW(TAG, "Loop detectado: node %s já está no path, descartando pacote", current_node_id.c_str());
                return;
            }
            
            // Verificar TTL antes de processar
            if (updated_pkt.ttl == 0)
            {
                ESP_LOGW(TAG, "TTL expirado, descartando pacote");
                return;
            }
            
            // Decrementa TTL e adiciona node ao path
            updated_pkt.ttl--;
            updated_pkt.trace.path.push_back(current_node_id);
            updated_pkt.trace.hop_count++;
            
            updated_pkt.trace.received_at_ms = now_ms;
            
            // Adicionar hop ao histórico
            Protocol::HopInfo hop;
            hop.node_id = current_node_id;
            hop.node_name = ""; // Pode ser preenchido pelo microserviço
            hop.timestamp_ms = now_ms;
            hop.transport = "MESH";
            updated_pkt.trace.hop_history.push_back(hop);
            
            // Node recebeu dado da mesh - mantém LED aceso durante 1 segundo antes de repassar
            ESP_LOGI(TAG, "Node %s recebeu dado da mesh (hop %u) - mantendo LED aceso por 1 segundo antes de repassar...", 
                     current_node_id.c_str(), updated_pkt.trace.hop_count);
            LedManager::set_led_on_for_duration(1000); // Mantém LED aceso por 1 segundo
            vTaskDelay(pdMS_TO_TICKS(1000)); // Aguarda 1 segundo
            
            // Envia pela malha; lógica "borda→UART" agora está em NetworkManager::send()
            ESP_LOGI(TAG, "Repassando dado após 1 segundo...");
            // LED pisca dentro de ESPNOWTransport::send() quando envia
            ESPNOWTransport::send(updated_pkt);
            return;
        }

        // demais destinos específicos — aqui entraria roteamento por vizinho
    }

} // namespace WetzelMesh
