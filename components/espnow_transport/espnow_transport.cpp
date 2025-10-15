#include "espnow_transport.hpp"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "router.hpp"
#include "led_manager.hpp"
#include "network_manager.hpp"

namespace WetzelMesh
{

    static const char *TAG = "ESPNOW";

    // endereço broadcast (6 bytes)
    static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    const uint8_t *ESPNOWTransport::broadcast_addr()
    {
        return kBroadcast;
    }

    static void espnow_recv_thunk(const esp_now_recv_info_t *info,
                                  const uint8_t *data, int len)
    {
        const uint8_t *mac = info ? info->src_addr : nullptr;
        int rssi = 0;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        if (info && info->rx_ctrl)
            rssi = info->rx_ctrl->rssi;
#else
        if (info)
            rssi = info->rx_ctrl.rssi;
#endif

        WetzelMesh::ESPNOWTransport::on_recv_cb(mac, data, len, rssi);
    }

    bool ESPNOWTransport::ensure_wifi_started()
    {
        ESP_ERROR_CHECK(esp_netif_init());
        (void)esp_event_loop_create_default(); // ignora erro se já criado
        (void)esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE)
        {
            ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
            return false;
        }
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_CONN)
        {
            ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        }
        return true;
    }

    bool ESPNOWTransport::ensure_channel_fixed()
    {
        // Quando não associado, fixe um canal estático compatível com a malha
        esp_err_t err = esp_wifi_set_channel(kESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "set_channel=%d: %s", kESPNOW_CHANNEL, esp_err_to_name(err));
            return false;
        }
        return true;
    }

    bool ESPNOWTransport::ensure_broadcast_peer()
    {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
        if (esp_now_is_peer_exist(kBroadcast))
            return true;
#endif
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, kBroadcast, 6);
        peer.ifidx = WIFI_IF_STA;
        peer.channel = kESPNOW_CHANNEL; // quando STA estiver associado, o rádio fica no canal do AP
        peer.encrypt = false;

        esp_err_t err = esp_now_add_peer(&peer);
        if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST)
        {
            ESP_LOGI(TAG, "Peer broadcast ativo (canal base %d)", kESPNOW_CHANNEL);
            return true;
        }
        ESP_LOGE(TAG, "add_peer(broadcast): %s", esp_err_to_name(err));
        return false;
    }

    void ESPNOWTransport::init()
    {
        ESP_LOGI(TAG, "Inicializando ESPNOW...");
        if (!ensure_wifi_started())
        {
            ESP_LOGE(TAG, "Wi-Fi STA nao iniciou; abortando ESPNOW");
            return;
        }

        // Se não associado a AP, travamos no canal fixo
        ensure_channel_fixed();

        // Reinit seguro
        (void)esp_now_deinit();
        esp_err_t err = esp_now_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
            return;
        }

        ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_thunk));
        ensure_broadcast_peer();

        ESP_LOGI(TAG, "ESPNOW iniciado (canal base %d)", kESPNOW_CHANNEL);
    }

    bool ESPNOWTransport::send(const Protocol::Packet &p)
    {
        // log de intenção de envio pela malha
        const std::string payload = Protocol::serialize(p);
        ESP_LOGI(TAG, "TX[MESH] %s -> %s (%u bytes)",
                 p.route.src.c_str(), p.route.dst.c_str(), (unsigned)payload.size());

        // Garante canal (quando não associado) e peer
        ensure_channel_fixed();
        if (!ensure_broadcast_peer())
        {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (!ensure_broadcast_peer())
            {
                ESP_LOGE(TAG, "Sem peer broadcast; cancelando TX[MESH]");
                return false;
            }
        }

        esp_err_t err = esp_now_send(broadcast_addr(),
                                     reinterpret_cast<const uint8_t *>(payload.data()),
                                     payload.size());
        if (err == ESP_ERR_ESPNOW_NOT_FOUND)
        {
            ESP_LOGW(TAG, "TX[MESH] peer nao encontrado; recriando...");
            ensure_channel_fixed();
            ensure_broadcast_peer();
            vTaskDelay(pdMS_TO_TICKS(20));
            err = esp_now_send(broadcast_addr(),
                               reinterpret_cast<const uint8_t *>(payload.data()),
                               payload.size());
        }
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "TX[MESH] erro: %s", esp_err_to_name(err));
            return false;
        }

        LedManager::blink(TrafficSource::MESH); // ATUALIZADO
        return true;
    }

    void ESPNOWTransport::on_recv_cb(const uint8_t * /*mac*/, const uint8_t *data, int len, int rssi)
    {
        if (len <= 0)
            return;

        std::string s(reinterpret_cast<const char *>(data), len);
        Protocol::Packet pkt;
        if (Protocol::parse(s, pkt))
        {
            ESP_LOGI(TAG, "RX[MESH] from=%s dst=%s rssi=%d (%d bytes)",
                     pkt.route.src.c_str(), pkt.route.dst.c_str(), rssi, len);

            if(pkt.type == Protocol::PacketType::EVENT && pkt.method == "HELLO")
                NetworkManager::on_hello(pkt.route.src, rssi);

            Router::handle_packet(pkt);
            LedManager::blink(TrafficSource::MESH); // ATUALIZADO
        }
        else
        {
            ESP_LOGW(TAG, "RX[MESH] pacote invalido (%d bytes)", len);
        }
    }

} // namespace WetzelMesh
