#include "test_packet_generator.hpp"
#include "protocol.hpp"
#include "network_manager.hpp"
#include "led_manager.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
    const char *TAG = "TESTGEN";

    void test_task(void *) {
        for (;;) {
            WetzelMesh::Protocol::Packet p{};
            p.route.src = "testgen";
            p.route.dst = "broadcast";
            p.body = R"({"ping":1})";  // Corrigido (não existe 'payload')

            WetzelMesh::LedManager::on_packet_received();  // Corrigido (namespace)
            WetzelMesh::NetworkManager::send(p);

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

namespace WetzelMesh {
void start_test_generation() {
    ESP_LOGI(TAG, "Iniciando gerador de pacotes de teste.");
    xTaskCreatePinnedToCore(test_task, "testgen", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
}
}
