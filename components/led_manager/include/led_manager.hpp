#pragma once
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace WetzelMesh
{

    class LedManager
    {
    public:
        static void init(bool isGateway);
        static void blinkDataLed();
        static void setConnectionLed(bool connected);

    private:
        static gpio_num_t dataLedPin;
        static gpio_num_t connectionLedPin;
        static bool gatewayMode;
    };

} // namespace WetzelMesh
