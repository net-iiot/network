#pragma once
#include <stdint.h>
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{

  class ESPNOWTransport
  {
  public:
    static void init();
    static bool send(const Protocol::Packet &p);
    static void on_recv_cb(const uint8_t *mac, const uint8_t *data, int len, int rssi);

  private:
    static bool ensure_wifi_started();
    static bool ensure_channel_fixed();
    static bool ensure_broadcast_peer();
    static const uint8_t *broadcast_addr();

    static constexpr uint8_t kESPNOW_CHANNEL = 1;
  };

} // namespace WetzelMesh
