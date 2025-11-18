#include "test_packet_generator.hpp"
#include "protocol.hpp"
#include "network_manager.hpp"
#include "led_manager.hpp"
#include "ble_transport.hpp"
#include "border_uart.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdlib>
#include <ctime>
#include <string>
#include <algorithm>

namespace WetzelMesh
{
    static const char *TAG = "TESTGEN";
    
    // Estado do token passing
    static bool s_has_token = false;
    static std::string s_token_from = "";
    static uint32_t s_token_hold_time_ms = 1000;

#ifndef CONFIG_WETZEL_TEST_MODE
    static void real_mode_task(void *)
    {
        // Node não gera dados próprios - apenas repassa dados recebidos
        // O gateway é quem gera e envia os dados
        ESP_LOGI(TAG, "Modo REAL: Node aguardando dados do gateway (não gera dados próprios)");
        for (;;)
        {
            vTaskDelay(pdMS_TO_TICKS(10000)); // Apenas mantém a task viva
        }
    }
#endif // CONFIG_WETZEL_TEST_MODE

    static void token_passing_task(void *)
    {
        // Aguarda alguns segundos para a rede estabilizar
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        std::string my_id = BLETransport::node_id();
        ESP_LOGI(TAG, "Token passing iniciado. Node: %s (aguardando token do Gateway)", my_id.c_str());
        
        for (;;)
        {
            const auto &neighbors = NetworkManager::neighbors();
            bool has_neighbors = !neighbors.empty();
            
            if (s_has_token)
            {
                ESP_LOGI(TAG, "TOKEN recebido de %s - mantendo por %u ms", 
                         s_token_from.c_str(), s_token_hold_time_ms);
                
                // Acende LED enquanto tem token
                LedManager::set_led_on_for_duration(s_token_hold_time_ms);
                
                // Aguarda o tempo configurado
                vTaskDelay(pdMS_TO_TICKS(s_token_hold_time_ms));
                
                // Apaga LED quando passa token
                // (set_led_on_for_duration já apaga automaticamente, mas garantimos)
                
                // Escolhe próximo node ou retorna para gateway
                std::string next_node;
                
                if (has_neighbors)
                {
                    // Se veio do gateway (via UART), passa para primeiro vizinho
                    if (s_token_from == "gateway" || s_token_from == "border")
                    {
                        next_node = neighbors[0].id;
                    }
                    else
                    {
                        // Escolhe primeiro vizinho que não seja o que enviou o token
                        for (const auto &nbr : neighbors)
                        {
                            if (nbr.id != s_token_from && nbr.id != my_id)
                            {
                                next_node = nbr.id;
                                break;
                            }
                        }
                        
                        // Se não encontrou outro, tenta retornar para gateway
                        if (next_node.empty())
                        {
                            // Verifica se é border node (tem UART)
                            if (BorderUart::is_enabled())
                            {
                                next_node = "gateway"; // Retorna para gateway via UART
                            }
                            else
                            {
                                // Escolhe qualquer vizinho (exceto ele mesmo)
                                for (const auto &nbr : neighbors)
                                {
                                    if (nbr.id != my_id)
                                    {
                                        next_node = nbr.id;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    
                    if (!next_node.empty())
                    {
                        Protocol::Packet token{};
                        token.type = Protocol::PacketType::EVENT;
                        token.method = "TOKEN";
                        token.route.src = my_id;
                        token.route.dst = next_node;
                        token.body = R"({"type":"token","from":")" + my_id + R"("})";
                        
                        ESP_LOGI(TAG, "Passando TOKEN para %s", next_node.c_str());
                        NetworkManager::send(token);
                        LedManager::blink(TrafficSource::MESH);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Nenhum vizinho disponível para passar token");
                    }
                }
                else
                {
                    // Sem vizinhos - se é border node, retorna para gateway
                    if (BorderUart::is_enabled())
                    {
                        Protocol::Packet token{};
                        token.type = Protocol::PacketType::EVENT;
                        token.method = "TOKEN";
                        token.route.src = my_id;
                        token.route.dst = "gateway";
                        token.body = R"({"type":"token","from":")" + my_id + R"("})";
                        
                        ESP_LOGI(TAG, "Retornando TOKEN para gateway (sem vizinhos)");
                        BorderUart::send_to_gateway(token);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Sem vizinhos e não é border node - token não pode ser passado");
                    }
                }
                
                s_has_token = false;
                s_token_from = "";
            }
            else
            {
                // Não tem token
                if (!has_neighbors)
                {
                    // Sem vizinhos = LED aceso (aguardando)
                    LedManager::set_node_joined(false); // Isso acende o LED
                }
                else
                {
                    // Tem vizinhos mas não tem token = LED apagado (aguardando token)
                    LedManager::set_node_joined(true); // Isso apaga o LED
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    void start_test_generation()
    {
#ifdef CONFIG_WETZEL_TEST_MODE
        ESP_LOGI(TAG, "Iniciando modo TESTE (Token Passing)...");
        s_token_hold_time_ms = 1000; // 1 segundo (padrão do teste)
        ESP_LOGI(TAG, "Tempo de token por node: %u ms", s_token_hold_time_ms);
        xTaskCreatePinnedToCore(token_passing_task, "token_test", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
#else
        ESP_LOGI(TAG, "Iniciando modo REAL (Produção)...");
        xTaskCreatePinnedToCore(real_mode_task, "testgen", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
#endif
    }
    
    // Função para receber token (chamada pelo NetworkManager)
    void on_token_received(const Protocol::Packet &token_pkt)
    {
        if (token_pkt.method == "TOKEN")
        {
            s_has_token = true;
            s_token_from = token_pkt.route.src;
            ESP_LOGI(TAG, "TOKEN recebido de %s", s_token_from.c_str());
        }
    }
    
    // Função para obter tempo de token (para uso externo)
    uint32_t get_token_hold_time_ms()
    {
        return s_token_hold_time_ms;
    }

} // namespace WetzelMesh