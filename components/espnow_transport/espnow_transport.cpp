#include "espnow_transport.hpp"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_idf_version.h"

#include "led_manager.hpp"
#include "network_manager.hpp"
#include "border_uart.hpp"
#include "ble_transport.hpp"

namespace NetworkMesh
{

    static const char *TAG = "ESPNOW";

    static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    uint8_t ESPNOWTransport::s_channel = 1;

    static void espnow_send_thunk(const wifi_tx_info_t *info, esp_now_send_status_t status);
    static void espnow_recv_thunk(const esp_now_recv_info_t *info, const uint8_t *data, int len);

    const uint8_t *ESPNOWTransport::broadcast_addr()
    {
        return kBroadcast;
    }

    bool ESPNOWTransport::ensure_wifi_started()
    {
        (void)esp_event_loop_create_default();
        (void)esp_netif_init();
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
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_CONN)
            ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(err));

        ESP_LOGI(TAG, "Wi-Fi STA iniciado.");
        return true;
    }

    uint8_t ESPNOWTransport::get_channel()
    {
        return s_channel;
    }

    bool ESPNOWTransport::ensure_channel_fixed()
    {
        s_channel = NetworkManager::get_wifi_channel();

        esp_err_t err = esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "set_channel=%d: %s", s_channel, esp_err_to_name(err));
            return false;
        }

        uint8_t ch = 0;
        wifi_second_chan_t sc = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&ch, &sc);
        ESP_LOGI(TAG, "Canal ESPNOW fixado em %u (Network ID: %s).",
                 (unsigned)ch, NetworkManager::get_network_id().c_str());
        return true;
    }

    bool ESPNOWTransport::ensure_broadcast_peer()
    {
        bool encryption_enabled = false;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
        if (esp_now_is_peer_exist(kBroadcast))
        {
            esp_now_peer_info_t peer{};
            memcpy(peer.peer_addr, kBroadcast, 6);
            peer.ifidx = WIFI_IF_STA;
            peer.channel = s_channel;
            peer.encrypt = encryption_enabled;
            (void)esp_now_mod_peer(&peer);
            return true;
        }
#endif

        esp_now_peer_info_t peer{};
        memcpy(peer.peer_addr, kBroadcast, 6);
        peer.ifidx = WIFI_IF_STA;
        peer.channel = s_channel;
        peer.encrypt = encryption_enabled;

        esp_err_t err = esp_now_add_peer(&peer);
        if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST)
        {
            ESP_LOGI(TAG, "Peer broadcast ativo (canal %u, Network: %s)",
                     s_channel, NetworkManager::get_network_id().c_str());
            return true;
        }
        ESP_LOGE(TAG, "add_peer(broadcast): %s", esp_err_to_name(err));
        return false;
    }

    void ESPNOWTransport::init()
    {
#ifdef CONFIG_NETWORK_IS_GATEWAY
        ESP_LOGW(TAG, "Init ignorado: este firmware esta em modo Gateway.");
        return;
#endif

        ESP_LOGI(TAG, "Inicializando ESPNOW...");
        if (!ensure_wifi_started())
        {
            ESP_LOGE(TAG, "Wi-Fi STA nao iniciou; abortando ESPNOW");
            return;
        }

        ensure_channel_fixed();

        (void)esp_now_deinit();

        esp_err_t err = esp_now_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
            return;
        }

        const uint8_t *pmk = NetworkManager::get_pmk();
        err = esp_now_set_pmk(pmk);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "esp_now_set_pmk falhou: %s", esp_err_to_name(err));
        else
            ESP_LOGI(TAG, "ESP-NOW encryption habilitado com PMK derivado do Network ID");

        ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_thunk));
        ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_thunk));

        ensure_broadcast_peer();

        ESP_LOGI(TAG, "ESPNOW iniciado (canal %u, encryption: %s, Network: %s)",
                 s_channel,
                 (err == ESP_OK) ? "SIM" : "NAO",
                 NetworkManager::get_network_id().c_str());
    }

    bool ESPNOWTransport::send(const Protocol::Packet &p)
    {
#ifdef CONFIG_NETWORK_IS_GATEWAY
        ESP_LOGW(TAG, "send() ignorado: Gateway nao usa ESPNOW.");
        return false;
#endif

        const std::string payload = Protocol::serialize(p);
        ESP_LOGI(TAG, "TX[MESH] %s -> %s (%u bytes)",
                 p.route.src.c_str(), p.route.dst.c_str(), (unsigned)payload.size());

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

        if (!(p.type == Protocol::PacketType::EVENT && p.method == "HELLO"))
            LedManager::blink(TrafficSource::MESH);

        return true;
    }

    static void espnow_send_thunk(const wifi_tx_info_t *info, esp_now_send_status_t st)
    {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
        const uint8_t *mac = (info ? info->des_addr : nullptr);
#else
        const uint8_t *mac = (info ? info->peer_addr : nullptr);
#endif

        ESP_LOGI(TAG, "TX-ACK[MESH] dst=%02X:%02X:%02X:%02X:%02X:%02X status=%s",
                 mac ? mac[0] : 0, mac ? mac[1] : 0, mac ? mac[2] : 0,
                 mac ? mac[3] : 0, mac ? mac[4] : 0, mac ? mac[5] : 0,
                 (st == ESP_NOW_SEND_SUCCESS) ? "OK" : "FAIL");
    }

    static void espnow_recv_thunk(const esp_now_recv_info_t *info, const uint8_t *data, int len)
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

        NetworkMesh::ESPNOWTransport::on_recv_cb(mac, data, len, rssi);
    }

    void ESPNOWTransport::on_recv_cb(const uint8_t *, const uint8_t *data, int len, int rssi)
    {
        if (len <= 0)
            return;

        std::string s(reinterpret_cast<const char *>(data), len);
        Protocol::Packet pkt;
        if (Protocol::parse(s, pkt))
        {
            ESP_LOGI(TAG, "RX[MESH] src=%s dst=%s rssi=%d (%d bytes)",
                     pkt.route.src.c_str(), pkt.route.dst.c_str(), rssi, len);

            if (pkt.type == Protocol::PacketType::EVENT && pkt.method == "HELLO")
            {
                std::string received_network_id = pkt.topology.network_id;
                std::string my_network_id = NetworkManager::get_network_id();

                if (!received_network_id.empty() && received_network_id != my_network_id)
                {
                    ESP_LOGW(TAG, "RX[MESH] HELLO ignorado: Network ID diferente (recebido: %s, esperado: %s)",
                             received_network_id.c_str(), my_network_id.c_str());
                    return;
                }

                NetworkManager::on_hello(pkt, rssi);
            }
            else
            {
                if (!pkt.trace.hop_history.empty())
                    pkt.trace.hop_history.back().rssi = rssi;

                if (pkt.type == Protocol::PacketType::REQUEST && pkt.method == "DISCOVERY")
                {
                    ESP_LOGI(TAG, "RX[MESH] DISCOVERY recebido - processando localmente primeiro...");
                    LedManager::blink(TrafficSource::MESH);
                    NetworkManager::handle_incoming(pkt);
                    NetworkManager::route_packet(pkt);
                    return;
                }

                if (pkt.type == Protocol::PacketType::RESPONSE &&
                    pkt.method == "DISCOVERY_RESPONSE" &&
                    pkt.route.dst == "gateway" &&
                    BorderUart::is_enabled())
                {
                    std::string my_id = BLETransport::node_id();
                    if (pkt.route.src != my_id && pkt.route.src != "border")
                    {
                        ESP_LOGI(TAG, "BORDER: DISCOVERY_RESPONSE de %s -> UART -> Gateway", pkt.route.src.c_str());
                        LedManager::blink(TrafficSource::MESH);
                        BorderUart::send_to_gateway(pkt);
                        return;
                    }
                }

                LedManager::blink(TrafficSource::MESH);
                auto &handler = NetworkManager::packet_handler();
                if (handler) handler(pkt);
            }
        }
        else
        {
            ESP_LOGW(TAG, "RX[MESH] pacote invalido (%d bytes)", len);
        }
    }

}
