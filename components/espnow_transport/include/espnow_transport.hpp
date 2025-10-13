#pragma once
#include <string>
#include "protocol.hpp"

namespace WetzelMesh
{

  class ESPNOWTransport
  {
  public:
    // Habilita Wi-Fi STA "dummy", inicializa ESP-NOW e cadastra peer broadcast
    static void init();

    // Envia um pacote pela malha (broadcast por enquanto)
    static bool send(const Protocol::Packet &p);

    // Callback de recepção (registra no esp-now)
    static void on_recv_cb(const uint8_t *mac, const uint8_t *data, int len);

  private:
    static void start();
  };

} // namespace WetzelMesh
