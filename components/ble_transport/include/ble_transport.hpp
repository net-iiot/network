#pragma once
#include <string>
#include <cstdint>
#include "protocol.hpp"

namespace NetworkMesh
{
    static constexpr const char *NETWORK_SERVICE_UUID_STR = "574D0001-AABB-CCDD-8899-102030405060";
    static constexpr const char *NETWORK_BUTTON_CHAR_UUID_STR = "574D0004-AABB-CCDD-8899-102030405060";

    struct __attribute__((packed)) ButtonPacket
    {
        uint8_t version;
        uint8_t event_type;
        uint8_t battery_pct;
        char button_id[16];
        uint8_t _reserved;
    };

    enum class ButtonEventType : uint8_t
    {
        PRESS = 0,
        LONG_PRESS = 1,
        DOUBLE_PRESS = 2,
    };

    class BLETransport
    {
    public:
        static void init(bool isGateway);
        static bool send(const Protocol::Packet &packet);
        static void on_receive_json(const std::string &jsonString);
        static bool isGateway() { return s_isGateway; }
        static std::string node_id();

        static bool s_isGateway;
        static uint16_t s_service_handle;
        static uint16_t s_rx_char_handle;
        static uint16_t s_button_char_handle;
        static void start_advertising();
        static void handle_button_write(const uint8_t *data, uint16_t len);

    private:
        static void start_gap_gatt();
        static void notify_tx(const std::string &data);
    };

}
