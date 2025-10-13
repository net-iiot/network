#include "test_packet_generator.hpp"
#include "protocol.hpp"
#include "network_manager.hpp"
#include "led_manager.hpp"
#include "ble_transport.hpp" // ✅ necessário para BLETransport::node_id()
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdlib>
#include <ctime>

namespace WetzelMesh
{

    static const char *TAG = "TESTGEN";

    static void test_task(void *)
    {
        std::srand(static_cast<unsigned>(time(nullptr)));
        int seq = 0;

        for (;;)
        {
            // Monta um pacote de teste válido
            Protocol::Packet pkt{};
            pkt.type = Protocol::PacketType::EVENT;
            pkt.method = "DATA";
            pkt.route.src = BLETransport::node_id(); // ✅ agora reconhecido
            pkt.route.dst = "gateway";               // o gateway vai receber via UART e repassar
            pkt.body = "{\"temp\":" + std::to_string(20 + (std::rand() % 10)) +
                       ",\"hum\":" + std::to_string(50 + (std::rand() % 20)) +
                       ",\"seq\":" + std::to_string(seq++) + "}";

            ESP_LOGI(TAG, "📦 Gerando pacote de teste: %s", pkt.body.c_str());

            // Indica atividade visual
            LedManager::on_packet_received();

            // Envia normalmente pela malha (UART/ESPNOW conforme o nó)
            NetworkManager::send(pkt);

            vTaskDelay(pdMS_TO_TICKS(1000)); // 1 s
        }
    }

    void start_test_generation()
    {
        ESP_LOGI(TAG, "Iniciando geração automática de pacotes de teste...");
        xTaskCreatePinnedToCore(test_task, "testgen", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
    }

} // namespace WetzelMesh
