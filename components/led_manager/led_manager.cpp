#include "led_manager.hpp"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace WetzelMesh
{
    static gpio_num_t kLedTraffic = GPIO_NUM_27; // gateway
    static gpio_num_t kLedNodeA = GPIO_NUM_26;   // node
    static gpio_num_t kLedNodeB = GPIO_NUM_25;   // node

    static bool s_isGateway = false;

    void LedManager::init(bool isGateway)
    {
        s_isGateway = isGateway;
        gpio_config_t io = {};
        io.intr_type = GPIO_INTR_DISABLE;
        io.mode = GPIO_MODE_OUTPUT;
        io.pin_bit_mask = (1ULL << kLedTraffic) | (1ULL << kLedNodeA) | (1ULL << kLedNodeB);
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io);

        gpio_set_level(kLedTraffic, 0);
        gpio_set_level(kLedNodeA, 0);
        gpio_set_level(kLedNodeB, 0);
    }

    void LedManager::blink()
    {
        gpio_num_t led = s_isGateway ? kLedTraffic : kLedNodeA;
        gpio_set_level(led, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        gpio_set_level(led, 0);
    }

    void LedManager::on_packet_received()
    {
        blink();
    }
}
