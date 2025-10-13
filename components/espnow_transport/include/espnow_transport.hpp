#pragma once
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{

  class ESPNOWTransport
  {
  public:
    // Inicializa ESPNOW, garante Wi-Fi STA ligado, fixa canal e cadastra peer broadcast
    static void init();

    // Envia um pacote pela malha (broadcast por enquanto)
    static bool send(const Protocol::Packet &p);

    // Callback de recepção (inclui RSSI real)
    static void on_recv_cb(const uint8_t *mac, const uint8_t *data, int len, int rssi);

  private:
    static bool ensure_wifi_started();   // garante Wi-Fi STA started
    static bool ensure_channel_fixed();  // fixa canal STA quando não associado
    static bool ensure_broadcast_peer(); // garante FF:FF:FF:FF:FF:FF cadastrado

    static inline const uint8_t *broadcast_addr();
  };

} // namespace WetzelMesh
