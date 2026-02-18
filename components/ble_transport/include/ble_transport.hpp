#pragma once
#include <string>
#include <cstdint>
#include "protocol.hpp"

namespace WetzelMesh
{
    // ─────────────────────────────────────────────────────────────────────────
    // UUIDs do serviço WetzelMesh (128-bit, little-endian conforme ESP-IDF)
    //
    // Serviço principal:  574D0001-AABB-CCDD-8899-102030405060
    // Char RX (write):    574D0002-AABB-CCDD-8899-102030405060
    // Char BUTTON (write):574D0004-AABB-CCDD-8899-102030405060
    //
    // O botão deve fazer scan por SERVICE_UUID e escrever em BUTTON_CHAR_UUID.
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr const char *WETZEL_SERVICE_UUID_STR  = "574D0001-AABB-CCDD-8899-102030405060";
    static constexpr const char *WETZEL_BUTTON_CHAR_UUID_STR = "574D0004-AABB-CCDD-8899-102030405060";

    // ─────────────────────────────────────────────────────────────────────────
    // Estrutura do pacote enviado pelo botão → node via BLE write (binário, 20 bytes)
    //
    // O botão acorda do deep sleep, conecta no node pelo SERVICE_UUID,
    // escreve ButtonPacket na BUTTON_CHAR_UUID e dorme novamente.
    // O node encaminha o evento via ESP-NOW → Gateway → microserviço dedicado.
    // ─────────────────────────────────────────────────────────────────────────
    struct __attribute__((packed)) ButtonPacket
    {
        uint8_t version;       // sempre 1
        uint8_t event_type;    // 0=press  1=long_press  2=double_press
        uint8_t battery_pct;   // 0–100 (% bateria)
        char    button_id[16]; // ID único do botão, null-padded  ex: "BTN-001"
        uint8_t _reserved;     // alinhamento / uso futuro
    }; // 20 bytes total

    // ─────────────────────────────────────────────────────────────────────────
    // Tipos de evento (campo event_type)
    // ─────────────────────────────────────────────────────────────────────────
    enum class ButtonEventType : uint8_t
    {
        PRESS        = 0,
        LONG_PRESS   = 1,
        DOUBLE_PRESS = 2,
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Transporte BLE — GATT server (periférico)
    // Todos os nodes inicializam BLE obrigatoriamente.
    // Gateway NÃO usa BLE.
    // ─────────────────────────────────────────────────────────────────────────
    class BLETransport
    {
    public:
        static void init(bool isGateway);
        static bool send(const Protocol::Packet &packet);
        static void on_receive_json(const std::string &jsonString);
        static bool isGateway() { return s_isGateway; }
        static std::string node_id();

    private:
        static void start_gap_gatt();
        static void start_advertising();
        static void notify_tx(const std::string &data);
        static void handle_button_write(const uint8_t *data, uint16_t len);

        static bool     s_isGateway;
        static uint16_t s_service_handle;
        static uint16_t s_rx_char_handle;
        static uint16_t s_button_char_handle;
    };

} // namespace WetzelMesh
