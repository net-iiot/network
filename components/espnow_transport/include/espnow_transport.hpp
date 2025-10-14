#pragma once
#include <stdint.h>
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{

  class ESPNOWTransport
  {
  public:
    // Inicializa o ESPNOW (Wi-Fi STA, canal e peer broadcast)
    static void init();

    // Envia um pacote pela malha (broadcast)
    static bool send(const Protocol::Packet &p);

    // Callback de RX chamado pelo driver
    static void on_recv_cb(const uint8_t *mac, const uint8_t *data, int len, int rssi);

  private:
    // Helpers internas – implementadas no .cpp
    static bool ensure_wifi_started();
    static bool ensure_channel_fixed();
    static bool ensure_broadcast_peer();
    static const uint8_t *broadcast_addr();

    // Canal fixo quando o STA não está associado a um AP
    static constexpr uint8_t kESPNOW_CHANNEL = 1;
  };

} // namespace WetzelMesh
