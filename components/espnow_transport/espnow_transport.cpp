#include "espnow_transport.hpp"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "router.hpp"
#include "led_manager.hpp"


namespace WetzelMesh
{

    static const char *TAG = "ESPNOW";

    static void espnow_recv_thunk(const esp_now_recv_info_t *info,
                                  const uint8_t *data, int len)
    {
        const uint8_t *mac = info ? info->src_addr : nullptr;
        WetzelMesh::ESPNOWTransport::on_recv_cb(mac, data, len);
    }

    void ESPNOWTransport::init()
    {
        // Wi-Fi STA “dummy” para habilitar o PHY (não conecta em AP)
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_ERROR_CHECK(esp_now_init());
        ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_thunk));

        // Peer broadcast (todos no alcance recebem)
        esp_now_peer_info_t peer = {};
        memset(&peer, 0, sizeof(peer));
        memset(peer.peer_addr, 0xFF, 6); // broadcast
        peer.ifidx = WIFI_IF_STA;
        peer.channel = 0; // canal atual
        peer.encrypt = false;
        esp_now_add_peer(&peer);

        ESP_LOGI(TAG, "ESPNOW iniciado (broadcast peer adicionado)");
    }

    bool ESPNOWTransport::send(const Protocol::Packet &p)
    {
        std::string payload = Protocol::serialize(p);
        esp_err_t err = esp_now_send(nullptr, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
        if (err != ESP_OK)
            return false;
        LedManager::on_packet_received();
        return true;
    }

    void ESPNOWTransport::on_recv_cb(const uint8_t * /*mac*/, const uint8_t *data, int len)
    {
        if (len <= 0)
            return;
        std::string s(reinterpret_cast<const char *>(data), len);

        Protocol::Packet pkt;
        if (Protocol::parse(s, pkt))
        {
            Router::handle_packet(pkt); // entra no roteador comum do projeto
            LedManager::on_packet_received();
        }
    }

} // namespace WetzelMesh
