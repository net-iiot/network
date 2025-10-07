#pragma once
#include "esp_err.h"
#include <functional>
#include <vector>
#include <cstdint>

namespace wetzelmesh
{

    class UARTBridge
    {
    public:
        using PacketHandler = std::function<void(const std::vector<uint8_t> &)>;

        esp_err_t init();
        esp_err_t sendPacket(const std::vector<uint8_t> &data);
        void setPacketHandler(PacketHandler handler);

    private:
        static void rxTask(void *arg);
        static uint32_t crc32(const uint8_t *data, size_t len);

        void handleIncoming(const uint8_t *data, size_t len);

        PacketHandler onPacket;
    };

}
