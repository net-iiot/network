#include "espnow_transport.hpp"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "router.hpp"
#include "led_manager.hpp"
#include "protocol.hpp"
#include "network_manager.hpp"

namespace WetzelMesh
{

    static const char *TAG = "ESPNOW";

    // Canal fixo para operação sem associação (ajuste se necessário)
    static constexpr uint8_t kESPNOW_CHANNEL = 1;
    static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    inline const uint8_t *ESPNOWTransport::broadcast_addr() { return kBroadcast; }

    // ----------------------------------------------------------------------------
    // Helpers privados
    // ----------------------------------------------------------------------------

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
        // netif + loop + STA default (idempotentes)
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE)
        {
            ESP_LOGE(TAG, "esp_wifi_init falhou: %s", esp_err_to_name(err));
            return false;
        }

        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_CONN)
        {
            ESP_LOGW(TAG, "esp_wifi_start retornou: %s", esp_err_to_name(err));
        }

        return true;
    }

    bool ESPNOWTransport::ensure_channel_fixed()
    {
        // Quando STA não está associado, fixe explicitamente um canal
        // para que ESPNOW saiba onde transmitir/receber.
        esp_err_t err = esp_wifi_set_channel(kESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao setar canal=%d: %s", kESPNOW_CHANNEL, esp_err_to_name(err));
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
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, kBroadcast, 6);
        peer.ifidx = WIFI_IF_STA;
        peer.channel = kESPNOW_CHANNEL; // **mesmo canal fixo**
        peer.encrypt = false;

        esp_err_t err = esp_now_add_peer(&peer);
        if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST)
        {
            ESP_LOGI(TAG, "Peer broadcast ativo no canal %d", kESPNOW_CHANNEL);
            return true;
        }

        ESP_LOGE(TAG, "Falha ao adicionar peer broadcast: %s", esp_err_to_name(err));
        return false;
    }

    // ----------------------------------------------------------------------------
    // API pública
    // ----------------------------------------------------------------------------

    void ESPNOWTransport::init()
    {
        ESP_LOGI(TAG, "Inicializando ESPNOW...");

        if (!ensure_wifi_started())
        {
            ESP_LOGE(TAG, "Wi-Fi STA não iniciou; abortando ESPNOW");
            return;
        }

        // Fixar canal antes de iniciar ESPNOW
        ensure_channel_fixed();

        // (Re)inicializa ESPNOW com segurança
        esp_now_deinit(); // ok chamar mesmo se não estiver iniciado
        esp_err_t err = esp_now_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_now_init falhou: %s", esp_err_to_name(err));
            return;
        }

        ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_thunk));

        // Garante peer broadcast no canal fixo
        ensure_broadcast_peer();

        ESP_LOGI(TAG, "ESPNOW iniciado no canal %d", kESPNOW_CHANNEL);
    }

    bool ESPNOWTransport::send(const Protocol::Packet &p)
    {
        // Garante que estamos com canal fixo e peer broadcast registrado
        ensure_channel_fixed();
        if (!ensure_broadcast_peer())
        {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (!ensure_broadcast_peer())
            {
                ESP_LOGE(TAG, "Sem peer broadcast; cancelando envio");
                return false;
            }
        }

        std::string payload = Protocol::serialize(p);

        // Use o endereço broadcast explicitamente (não nullptr)
        esp_err_t err = esp_now_send(broadcast_addr(),
                                     reinterpret_cast<const uint8_t *>(payload.data()),
                                     payload.size());

        if (err == ESP_ERR_ESPNOW_NOT_FOUND)
        {
            ESP_LOGW(TAG, "Peer não encontrado; recriando e tentando novamente...");
            ensure_channel_fixed();
            ensure_broadcast_peer();
            vTaskDelay(pdMS_TO_TICKS(20));
            err = esp_now_send(broadcast_addr(),
                               reinterpret_cast<const uint8_t *>(payload.data()),
                               payload.size());
        }

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha no envio ESPNOW: %s", esp_err_to_name(err));
            return false;
        }

        LedManager::on_packet_received();
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
            // Atualiza vizinho quando for HELLO
            if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "HELLO")
            {
                NetworkManager::on_hello(pkt.route.src, rssi);
            }

            ESP_LOGI(TAG, "📡 RX ESPNOW [%s] RSSI=%d", pkt.route.src.c_str(), rssi);

            Router::handle_packet(pkt);
            LedManager::on_packet_received();
        }
        else
        {
            ESP_LOGW(TAG, "Pacote ESPNOW inválido recebido (%d bytes)", len);
        }
    }

} // namespace WetzelMesh
