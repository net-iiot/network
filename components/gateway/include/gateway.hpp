#pragma once
#include <string>
#include "driver/uart.h"
#include "driver/gpio.h" // para gpio_num_t / GPIO_NUM_xx

namespace WetzelMesh
{

    // Forward declaration para evitar include de protocol.hpp aqui
    namespace Protocol
    {
        struct Packet;
    }

    class Gateway
    {
    public:
        static void init();                               // Wi-Fi + HTTP + UART bridge
        static void listen_task(void *);                  // UART RX -> HTTP -> UART TX
        static void send(const Protocol::Packet &packet); // UART TX

        static void init_wifi(const char *ssid, const char *pass);
        static bool http_post_json(const std::string &url, const std::string &body, std::string &out_resp);

        static constexpr uart_port_t UART_PORT = UART_NUM_1;
        static constexpr gpio_num_t TX_PIN = GPIO_NUM_13;
        static constexpr gpio_num_t RX_PIN = GPIO_NUM_15;
        static constexpr int BAUD = 115200;
        static constexpr int BUF_SIZE = 2048;
    };

} // namespace WetzelMesh
