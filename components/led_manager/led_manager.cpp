#include "led_manager.hpp"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace WetzelMesh
{
    static gpio_num_t kLedTraffic = GPIO_NUM_27; // gateway
    static gpio_num_t kLedNodeA = GPIO_NUM_26;   // node
    static gpio_num_t kLedNodeB = GPIO_NUM_25;   // node (reservado)

    static bool s_isGateway = false;
    static bool s_nodeJoined = false;
    static bool s_gatewayConnected = false;

    static inline void set_level(gpio_num_t pin, int on)
    {
        gpio_set_level(pin, on ? 1 : 0);
    }

    static void refresh_leds()
    {
        if (s_isGateway)
        {
            // Gateway: ON enquanto NÃO conectado ao servidor; OFF após conectado
            set_level(kLedTraffic, s_gatewayConnected ? 0 : 1);
            // Nodes LEDs ficam apagados no gateway
            set_level(kLedNodeA, 0);
            set_level(kLedNodeB, 0);
        }
        else
        {
            // Node: ON enquanto NÃO joined; OFF após joined
            set_level(kLedNodeA, s_nodeJoined ? 0 : 1);
            // LED extra não usado por enquanto
            set_level(kLedNodeB, 0);
            // LED Traffic do gateway fica apagado no node
            set_level(kLedTraffic, 0);
        }
    }

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

        // Estados iniciais: "desconectado"
        s_nodeJoined = false;
        s_gatewayConnected = false;
        refresh_leds();
    }

    void LedManager::blink()
    {
        // Pisca no LED principal do papel atual
        gpio_num_t led = s_isGateway ? kLedTraffic : kLedNodeA;

        // Só pisca durante tráfego; estado fixo volta em seguida
        set_level(led, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        set_level(led, 0);

        // Reafirma estado fixo após o blink
        refresh_leds();
    }

    void LedManager::on_packet_received()
    {
        blink();
    }

    void LedManager::set_node_joined(bool joined)
    {
        s_nodeJoined = joined;
        refresh_leds();
    }

    void LedManager::set_gateway_server_connected(bool connected)
    {
        s_gatewayConnected = connected;
        refresh_leds();
    }
}
