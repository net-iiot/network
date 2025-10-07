#include "led_manager.hpp"
#include "esp_log.h"

using namespace WetzelMesh;

gpio_num_t LedManager::dataLedPin = GPIO_NUM_NC;
gpio_num_t LedManager::connectionLedPin = GPIO_NUM_NC;
bool LedManager::gatewayMode = false;

void LedManager::init(bool isGateway)
{
    gatewayMode = isGateway;
    dataLedPin = isGateway ? GPIO_NUM_27 : GPIO_NUM_26;
    connectionLedPin = isGateway ? GPIO_NUM_NC : GPIO_NUM_25;

    gpio_reset_pin(dataLedPin);
    gpio_set_direction(dataLedPin, GPIO_MODE_OUTPUT);

    if (!isGateway)
    {
        gpio_reset_pin(connectionLedPin);
        gpio_set_direction(connectionLedPin, GPIO_MODE_OUTPUT);
        gpio_set_level(connectionLedPin, 0);
    }

    ESP_LOGI("LedManager", "LEDs inicializados (modo: %s)", isGateway ? "Gateway" : "Node");
}

void LedManager::blinkDataLed()
{
    gpio_set_level(dataLedPin, 1);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(dataLedPin, 0);
}

void LedManager::setConnectionLed(bool connected)
{
    if (!gatewayMode && connectionLedPin != GPIO_NUM_NC)
    {
        gpio_set_level(connectionLedPin, connected ? 1 : 0);
    }
}
