#include "test_packet_generator.hpp"
#include "network_manager.hpp"
#include "protocol.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TEST_GEN";
static const TickType_t PERIOD = pdMS_TO_TICKS(10000);

static void test_task(void *arg)
{
    (void)arg;
    unsigned counter = 0;
    while (true)
    {
        vTaskDelay(PERIOD);
        WetzelMesh::Protocol::Packet pkt;
        ESP_LOGI(TAG, "Gerando paco-teste %u", counter++);
        WetzelMesh::NetworkManager::send(pkt);
    }
}

namespace WetzelMesh
{
    void start_test_generation()
    {
        xTaskCreate(&test_task, "test_pkt_gen", 4096, NULL, 5, NULL);
    }
}
