#include "ble_transport.hpp"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "led_manager.hpp"
#include "network_manager.hpp"
#include "protocol.hpp"
#include <string.h>
#include <cstring>
#include <vector>
#include <set>

using namespace NetworkMesh;

static const char *TAG = "BLE";

static const uint8_t SERVICE_UUID[16]     = {0x60,0x50,0x40,0x30,0x20,0x10,0x99,0x88,0xDD,0xCC,0xBB,0xAA,0x01,0x00,0x4D,0x57};
static const uint8_t RX_CHAR_UUID[16]     = {0x60,0x50,0x40,0x30,0x20,0x10,0x99,0x88,0xDD,0xCC,0xBB,0xAA,0x02,0x00,0x4D,0x57};
static const uint8_t BUTTON_CHAR_UUID[16] = {0x60,0x50,0x40,0x30,0x20,0x10,0x99,0x88,0xDD,0xCC,0xBB,0xAA,0x04,0x00,0x4D,0x57};

static const char *BLE_DEVICE_NAME = "NetworkMesh";

bool     BLETransport::s_isGateway         = false;
uint16_t BLETransport::s_service_handle    = 0;
uint16_t BLETransport::s_rx_char_handle    = 0;
uint16_t BLETransport::s_button_char_handle = 0;

static uint16_t g_tx_handle  = 0;
static esp_gatt_if_t g_gatts_if = ESP_GATT_IF_NONE;

static esp_ble_adv_params_t s_adv_params = {};
static bool s_adv_data_set  = false;
static bool s_advertising   = false;

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp        = false,
    .include_name        = false,
    .include_txpower     = false,
    .min_interval        = 0x0020,
    .max_interval        = 0x0040,
    .appearance          = 0x00,
    .manufacturer_len    = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len    = 0,
    .p_service_data      = nullptr,
    .service_uuid_len    = sizeof(SERVICE_UUID),
    .p_service_uuid      = (uint8_t *)SERVICE_UUID,
    .flag                = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static std::set<std::string> s_bonded_devices;
static const char *BONDED_NS  = "ble_bonded";
static const char *BONDED_KEY = "devices";

static std::string mac_to_str(const uint8_t *mac)
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

static void save_bonded_devices()
{
    nvs_handle_t h;
    if (nvs_open(BONDED_NS, NVS_READWRITE, &h) != ESP_OK) return;
    std::string list;
    for (const auto &m : s_bonded_devices) list += m + "\n";
    nvs_set_str(h, BONDED_KEY, list.c_str());
    nvs_commit(h);
    nvs_close(h);
}

static void load_bonded_devices()
{
    nvs_handle_t h;
    if (nvs_open(BONDED_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = 0;
    if (nvs_get_str(h, BONDED_KEY, nullptr, &sz) != ESP_OK || sz == 0) { nvs_close(h); return; }
    std::vector<char> buf(sz);
    if (nvs_get_str(h, BONDED_KEY, buf.data(), &sz) == ESP_OK)
    {
        std::string mac;
        for (char c : std::string(buf.data()))
        {
            if (c == '\n') { if (!mac.empty()) { s_bonded_devices.insert(mac); mac.clear(); } }
            else           { mac += c; }
        }
        if (!mac.empty()) s_bonded_devices.insert(mac);
    }
    nvs_close(h);
}

static void add_bonded_device(const uint8_t *mac)
{
    auto s = mac_to_str(mac);
    if (s_bonded_devices.insert(s).second)
        save_bonded_devices();
}

void BLETransport::handle_button_write(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(ButtonPacket))
    {
        ESP_LOGW(TAG, "BUTTON: pacote muito curto (%d bytes, esperado %d)", len, (int)sizeof(ButtonPacket));
        return;
    }

    ButtonPacket pkt;
    memcpy(&pkt, data, sizeof(ButtonPacket));

    if (pkt.version != 1)
    {
        ESP_LOGW(TAG, "BUTTON: versao desconhecida %d", pkt.version);
        return;
    }

    char btn_id[17] = {};
    memcpy(btn_id, pkt.button_id, 16);

    const char *event_names[] = {"press", "long_press", "double_press"};
    const char *event_name = (pkt.event_type < 3) ? event_names[pkt.event_type] : "unknown";

    ESP_LOGI(TAG, "BUTTON_EVENT: id=%s event=%s battery=%d%%", btn_id, event_name, pkt.battery_pct);

    Protocol::Packet out;
    out.type   = Protocol::PacketType::EVENT;
    out.method = "BUTTON_EVENT";
    out.route.src  = std::string(btn_id);
    out.route.dst  = "server";

    char body_buf[256];
    snprintf(body_buf, sizeof(body_buf),
             "{\"button_id\":\"%s\",\"event_type\":%d,\"event_name\":\"%s\",\"battery_pct\":%d,\"node_id\":\"%s\"}",
             btn_id, static_cast<int>(pkt.event_type), event_name,
             static_cast<int>(pkt.battery_pct), node_id().c_str());
    out.body = std::string(body_buf);

    auto &handler = NetworkManager::packet_handler();
    if (handler) handler(out);
}

static void maybe_start_advertising()
{
    if (!s_advertising && s_adv_data_set)
    {
        if (esp_ble_gap_start_advertising(&s_adv_params) == ESP_OK)
        {
            s_advertising = true;
            ESP_LOGI(TAG, "Advertising iniciado");
        }
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_data_set = true;
        maybe_start_advertising();
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(TAG, "Falha ao iniciar ADV: 0x%02x", param->adv_start_cmpl.status);
            s_advertising = false;
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_advertising = false;
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success)
            add_bonded_device(param->ble_security.auth_cmpl.bd_addr);
        break;

    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTS_REG_EVT:
    {
        g_gatts_if = gatts_if;
        ESP_LOGI(TAG, "GATT registrado (if=%d), criando servico...", gatts_if);
        esp_ble_gap_set_device_name(BLE_DEVICE_NAME);

        esp_gatt_srvc_id_t service_id = {};
        service_id.is_primary    = true;
        service_id.id.inst_id    = 0;
        service_id.id.uuid.len   = ESP_UUID_LEN_128;
        memcpy(service_id.id.uuid.uuid.uuid128, SERVICE_UUID, 16);
        ESP_ERROR_CHECK(esp_ble_gatts_create_service(gatts_if, &service_id, 8));
        break;
    }

    case ESP_GATTS_CREATE_EVT:
    {
        BLETransport::s_service_handle = param->create.service_handle;
        ESP_LOGI(TAG, "Servico criado (handle=%d), adicionando RX char...", BLETransport::s_service_handle);

        esp_bt_uuid_t rx_uuid;
        rx_uuid.len = ESP_UUID_LEN_128;
        memcpy(rx_uuid.uuid.uuid128, RX_CHAR_UUID, 16);

        esp_ble_gatts_add_char(
            BLETransport::s_service_handle,
            &rx_uuid,
            ESP_GATT_PERM_WRITE,
            ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
            nullptr, nullptr);
        break;
    }

    case ESP_GATTS_ADD_CHAR_EVT:
    {
        uint16_t attr_handle = param->add_char.attr_handle;

        if (BLETransport::s_rx_char_handle == 0)
        {
            BLETransport::s_rx_char_handle = attr_handle;
            ESP_LOGI(TAG, "RX char adicionada (handle=%d), adicionando BUTTON char...", attr_handle);

            esp_bt_uuid_t btn_uuid;
            btn_uuid.len = ESP_UUID_LEN_128;
            memcpy(btn_uuid.uuid.uuid128, BUTTON_CHAR_UUID, 16);

            esp_ble_gatts_add_char(
                BLETransport::s_service_handle,
                &btn_uuid,
                ESP_GATT_PERM_WRITE,
                ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
                nullptr, nullptr);
        }
        else if (BLETransport::s_button_char_handle == 0)
        {
            BLETransport::s_button_char_handle = attr_handle;
            ESP_LOGI(TAG, "BUTTON char adicionada (handle=%d), iniciando servico...", attr_handle);
            esp_ble_gatts_start_service(BLETransport::s_service_handle);
        }
        break;
    }

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "Servico GATT iniciado");
        BLETransport::start_advertising();
        break;

    case ESP_GATTS_CONNECT_EVT:
    {
        ESP_LOGI(TAG, "BLE conectado: conn_id=%d MAC=%s",
                 param->connect.conn_id,
                 mac_to_str(param->connect.remote_bda).c_str());
        if (!BLETransport::isGateway())
            LedManager::set_node_joined(true);
        break;
    }

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "BLE desconectado: conn_id=%d", param->disconnect.conn_id);
        if (!BLETransport::isGateway())
            LedManager::set_node_joined(false);
        s_advertising = false;
        maybe_start_advertising();
        break;

    case ESP_GATTS_WRITE_EVT:
    {
        uint16_t handle = param->write.handle;
        uint16_t len    = param->write.len;
        const uint8_t *data = param->write.value;

        if (len == 0) break;

        if (handle == BLETransport::s_button_char_handle)
        {
            LedManager::blink(TrafficSource::MESH);
            BLETransport::handle_button_write(data, len);
        }
        else if (handle == BLETransport::s_rx_char_handle)
        {
            std::string json(reinterpret_cast<const char *>(data), len);
            Protocol::Packet packet;
            if (Protocol::parse(json, packet))
            {
                ESP_LOGI(TAG, "BLE RX: %s -> %s", packet.route.src.c_str(), packet.route.dst.c_str());
                LedManager::blink(TrafficSource::MESH);
                NetworkManager::handle_incoming(packet);
            }
            else
            {
                ESP_LOGW(TAG, "BLE RX: JSON invalido");
            }
        }
        break;
    }

    default:
        break;
    }
}

void BLETransport::start_gap_gatt()
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = ESP_BT_MODE_BLE;

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "Falha ao liberar Classic BT: %s", esp_err_to_name(err));

    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(err);

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err == ESP_ERR_INVALID_ARG)
    {
        (void)esp_bt_controller_disable();
        (void)esp_bt_controller_deinit();
        bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        bt_cfg.mode = ESP_BT_MODE_BTDM;
        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    load_bonded_devices();

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0x55));
}

void BLETransport::start_advertising()
{
    s_adv_params.adv_int_min       = 0x20;
    s_adv_params.adv_int_max       = 0x40;
    s_adv_params.adv_type          = ADV_TYPE_IND;
    s_adv_params.own_addr_type     = BLE_ADDR_TYPE_PUBLIC;
    s_adv_params.channel_map       = ADV_CHNL_ALL;
    s_adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

    s_adv_data_set = false;
    ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&s_adv_data));
}

std::string BLETransport::node_id()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[32];
    snprintf(buf, sizeof(buf), "node-%02X%02X%02X", mac[3], mac[4], mac[5]);
    return std::string(buf);
}

void BLETransport::init(bool isGateway)
{
    s_isGateway          = isGateway;
    s_service_handle     = 0;
    s_rx_char_handle     = 0;
    s_button_char_handle = 0;

    ESP_LOGI(TAG, "Inicializando BLE (Node)");
    start_gap_gatt();

    if (!s_isGateway)
        LedManager::set_node_joined(false);
}

void BLETransport::notify_tx(const std::string &data)
{
    if (g_tx_handle == 0) return;
    esp_ble_gatts_send_indicate(g_gatts_if, 0, g_tx_handle,
                                data.size(), (uint8_t *)data.data(), false);
}

bool BLETransport::send(const Protocol::Packet &packet)
{
    std::string json = Protocol::serialize(packet);
    notify_tx(json);
    return true;
}

void BLETransport::on_receive_json(const std::string &jsonString)
{
    Protocol::Packet pkt;
    if (Protocol::parse(jsonString, pkt))
    {
        auto &handler = NetworkManager::packet_handler();
        if (handler) handler(pkt);
    }
    else
        ESP_LOGW(TAG, "JSON BLE invalido");
}
