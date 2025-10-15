#pragma once
#include <functional>
#include <string>
#include "protocol.hpp"

namespace WetzelMesh {

class BorderUart {
public:
    using RxHandler = std::function<void(const Protocol::Packet&)>;

    static bool init();
    static bool is_enabled();
    static void set_rx_handler(RxHandler cb);

    // Envia JSON/Packet ao gateway pela UART
    static bool send_to_gateway(const Protocol::Packet& pkt);

    // ---------- Tornados públicos para resolver o erro de acesso ----------
    // Handshake simples PING->PONG; retorna true se ok dentro do timeout.
    static bool do_handshake(unsigned timeout_ms);

    // Tarefa de RX bloqueante (criada após handshake ok)
    static void uart_listen_task(void*);

    // Escrita com framing <len>\n<json>
    static bool uart_write_json(const std::string& json);

private:
    // N/A
};

} // namespace WetzelMesh
