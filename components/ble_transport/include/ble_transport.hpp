#pragma once
#include <string>
#include "esp_log.h"
#include "protocol.hpp"

namespace WetzelMesh
{

    class BLETransport
    {
    public:
        static void init();
        static void send(const Protocol::Packet &packet);

    private:
        static void advertise();
        static void on_receive(const std::string &data);
    };

} // namespace WetzelMesh
