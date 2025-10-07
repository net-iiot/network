#pragma once
#include <string>
#include "esp_log.h"
#include "driver/uart.h"
#include "protocol.hpp"

namespace WetzelMesh
{

    class Gateway
    {
    public:
        static void init();
        static void send(const Protocol::Packet &packet);
        static void listen_task(void *param);

    private:
        static constexpr uart_port_t UART_PORT = UART_NUM_1;
        static constexpr int TX_PIN = 13;
        static constexpr int RX_PIN = 15;
        static constexpr int BUF_SIZE = 2048;
    };

} // namespace WetzelMesh
