#include "led_manager.hpp"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace WetzelMesh
{
    static gpio_num_t kLedTraffic = GPIO_NUM_25; // LED primário: GW=ServerStatus, NODE=MeshStatus
    static gpio_num_t kLedNodeA = GPIO_NUM_26;   // LED secundário: NODE=UART Status
    // kLedNodeB removido para focar em 25 e 26

    static bool s_isGateway = false;
    static bool s_nodeJoined = false;
    static bool s_gatewayConnected = false;
    static bool s_nodeUartEnabled = false; // Novo estado para UART no Node

    static inline void set_level(gpio_num_t pin, int on)
    {
        gpio_set_level(pin, on ? 1 : 0);
    }

    static void refresh_leds()
    {
        if (s_isGateway)
        {
            // Gateway:
            // Pin 25 (kLedTraffic) -> Server Connection
            // ON se NÃO conectado; OFF se conectado. Se passar dados, pisca (tratado em blink)
            set_level(kLedTraffic, s_gatewayConnected ? 0 : 1);

            // Pin 26 (kLedNodeA) -> Não utilizado no modo Gateway.
            set_level(kLedNodeA, 0);
        }
        else
        {
            // Node:
            // Pin 25 (kLedTraffic) -> Mesh/ESPNOW (Node Joined)
            // ON se NÃO conectado (joined=false); OFF se conectado. Se passar dados, pisca (tratado em blink)
            set_level(kLedTraffic, s_nodeJoined ? 0 : 1);

            // Pin 26 (kLedNodeA) -> UART Border status
            // ON se UART NÃO está habilitada; OFF se UART está habilitada.
            set_level(kLedNodeA, s_nodeUartEnabled ? 0 : 1);
        }
    }

    void LedManager::init(bool isGateway)
    {
        s_isGateway = isGateway;

        gpio_config_t io = {};
        io.intr_type = GPIO_INTR_DISABLE;
        io.mode = GPIO_MODE_OUTPUT;
        // Configura apenas 25 e 26
        io.pin_bit_mask = (1ULL << kLedTraffic) | (1ULL << kLedNodeA);
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io);

        // Estados iniciais: "desconectado"
        s_nodeJoined = false;
        s_gatewayConnected = false;
        s_nodeUartEnabled = false;
        refresh_leds();
    }

    void LedManager::blink(TrafficSource source)
    {
        gpio_num_t led_to_blink = GPIO_NUM_MAX; // Inválido por padrão

        if (s_isGateway)
        {
            // Gateway: Todo o tráfego pisca no LED 25 (kLedTraffic)
            led_to_blink = kLedTraffic;
        }
        else
        {
            // Node:
            // MESH/BLE (MESH/SERVER) pisca no LED 25 (kLedTraffic)
            if (source == TrafficSource::MESH || source == TrafficSource::SERVER)
            {
                led_to_blink = kLedTraffic;
            }
            // UART pisca no LED 26 (kLedNodeA)
            else if (source == TrafficSource::UART)
            {
                led_to_blink = kLedNodeA;
            }
        }

        if (led_to_blink != GPIO_NUM_MAX)
        {
            // Pisca (estado fixo volta em seguida)
            set_level(led_to_blink, 1);
            vTaskDelay(pdMS_TO_TICKS(30));
            set_level(led_to_blink, 0);

            // Reafirma estado fixo após o blink
            refresh_leds();
        }
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

    void LedManager::set_uart_enabled(bool enabled)
    {
        s_nodeUartEnabled = enabled;
        refresh_leds();
    }
}