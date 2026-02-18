#include "ble_transport.hpp"
#include "router.hpp"
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

using namespace WetzelMesh;

static const char *TAG = "BLE";

// ─────────────────────────────────────────────────────────────────────────────
// UUIDs (little-endian, conforme ESP-IDF 128-bit UUID)
//
// Serviço:     574D0001-AABB-CCDD-8899-102030405060
// RX char:     574D0002-AABB-CCDD-8899-102030405060
// BUTTON char: 574D0004-AABB-CCDD-8899-102030405060
// ─────────────────────────────────────────────────────────────────────────────
static const uint8_t SERVICE_UUID[16]     = {0x57,0x4D,0x00,0x01,0xAA,0xBB,0xCC,0xDD,0x88,0x99,0x10,0x20,0x30,0x40,0x50,0x60};
static const uint8_t RX_CHAR_UUID[16]     = {0x57,0x4D,0x00,0x02,0xAA,0xBB,0xCC,0xDD,0x88,0x99,0x10,0x20,0x30,0x40,0x50,0x60};
static const uint8_t BUTTON_CHAR_UUID[16] = {0x57,0x4D,0x00,0x04,0xAA,0xBB,0xCC,0xDD,0x88,0x99,0x10,0x20,0x30,0x40,0x50,0x60};

// Nome BLE fixo (não configurável — todos os nodes usam o mesmo)
static const char *BLE_DEVICE_NAME = "WetzelMesh";

// ─────────────────────────────────────────────────────────────────────────────
// Statics
// ─────────────────────────────────────────────────────────────────────────────
bool     BLETransport::s_isGateway         = false;
uint16_t BLETransport::s_service_handle    = 0;
uint16_t BLETransport::s_rx_char_handle    = 0;
uint16_t BLETransport::s_button_char_handle = 0;

static uint16_t g_tx_handle  = 0;
static esp_gatt_if_t g_gatts_if = ESP_GATT_IF_NONE;

// ─────────────────────────────────────────────────────────────────────────────
// Advertising
// ─────────────────────────────────────────────────────────────────────────────
static esp_ble_adv_params_t s_adv_params = {};
static bool s_adv_data_set  = false;
static bool s_advertising   = false;

// Advertising anuncia o SERVICE_UUID — botão faz scan por ele
// include_name = false → invisível em scans genéricos, mas localizável via UUID
static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp        = false,
    .include_name        = false,
    .include_txpower     = false,
    .min_interval        = 0x0020, // 20ms
    .max_interval        = 0x0040, // 40ms
    .appearance          = 0x00,
    .manufacturer_len    = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len    = 0,
    .p_service_data      = nullptr,
    .service_uuid_len    = sizeof(SERVICE_UUID),
    .p_service_uuid      = (uint8_t *)SERVICE_UUID,
    .flag                = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

// ─────────────────────────────────────────────────────────────────────────────
// Bonding
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// Button event handler
// Chamado quando o botão escreve na BUTTON_CHAR_UUID.
// Converte ButtonPacket → Protocol::Packet (EVENT/BUTTON_EVENT) → Router.
// ─────────────────────────────────────────────────────────────────────────────
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
        ESP_LOGW(TAG, "BUTTON: versão desconhecida %d", pkt.version);
        return;
    }

    // Garante null-termination do button_id
    char btn_id[17] = {};
    memcpy(btn_id, pkt.button_id, 16);

    const char *event_names[] = {"press", "long_press", "double_press"};
    const char *event_name = (pkt.event_type < 3) ? event_names[pkt.event_type] : "unknown";

    ESP_LOGI(TAG, "BUTTON_EVENT: id=%s event=%s battery=%d%%", btn_id, event_name, pkt.battery_pct);

    // Monta Protocol::Packet EVENT
    Protocol::Packet out;
    out.type   = Protocol::PacketType::EVENT;
    out.method = "BUTTON_EVENT";
    out.route.src  = std::string(btn_id);
    out.route.dst  = "gateway";

    // Body JSON com todos os campos relevantes
    char body_buf[256];
    snprintf(body_buf, sizeof(body_buf),
             "{\"button_id\":\"%s\",\"event_type\":%d,\"event_name\":\"%s\",\"battery_pct\":%d,\"node_id\":\"%s\"}",
             btn_id, static_cast<int>(pkt.event_type), event_name,
             static_cast<int>(pkt.battery_pct), node_id().c_str());
    out.body = std::string(body_buf);

    Router::handle_packet(out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Advertising helper
// ─────────────────────────────────────────────────────────────────────────────
static void maybe_start_advertising()
{
    if (!s_advertising && s_adv_data_set)
    {
        if (esp_ble_gap_start_advertising(&s_adv_params) == ESP_OK)
        {
            s_advertising = true;
            ESP_LOGI(TAG, "Advertising iniciado (UUID: 574D0001-AABB-CCDD-8899-102030405060)");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GAP handler
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// GATTS handler
// ─────────────────────────────────────────────────────────────────────────────
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
    // ── Registro do app GATT → cria serviço ────────────────────────────────
    case ESP_GATTS_REG_EVT:
    {
        g_gatts_if = gatts_if;
        ESP_LOGI(TAG, "GATT registrado (if=%d), criando serviço...", gatts_if);
        esp_ble_gap_set_device_name(BLE_DEVICE_NAME);

        esp_gatt_srvc_id_t service_id = {};
        service_id.is_primary    = true;
        service_id.id.inst_id    = 0;
        service_id.id.uuid.len   = ESP_UUID_LEN_128;
        memcpy(service_id.id.uuid.uuid.uuid128, SERVICE_UUID, 16);
        ESP_ERROR_CHECK(esp_ble_gatts_create_service(gatts_if, &service_id, 8));
        break;
    }

    // ── Serviço criado → adiciona characteristic RX ─────────────────────────
    case ESP_GATTS_CREATE_EVT:
    {
        BLETransport::s_service_handle = param->create.service_handle;
        ESP_LOGI(TAG, "Serviço criado (handle=%d), adicionando RX char...", BLETransport::s_service_handle);

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

    // ── Characteristic adicionada ────────────────────────────────────────────
    case ESP_GATTS_ADD_CHAR_EVT:
    {
        uint16_t attr_handle = param->add_char.attr_handle;

        // Primeira char adicionada = RX
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
        // Segunda char adicionada = BUTTON
        else if (BLETransport::s_button_char_handle == 0)
        {
            BLETransport::s_button_char_handle = attr_handle;
            ESP_LOGI(TAG, "BUTTON char adicionada (handle=%d), iniciando serviço...", attr_handle);
            esp_ble_gatts_start_service(BLETransport::s_service_handle);
        }
        break;
    }

    // ── Serviço iniciado → começa advertising ───────────────────────────────
    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "Serviço GATT iniciado");
        BLETransport::start_advertising();
        break;

    // ── Conexão ─────────────────────────────────────────────────────────────
    case ESP_GATTS_CONNECT_EVT:
    {
        ESP_LOGI(TAG, "BLE conectado: conn_id=%d MAC=%s",
                 param->connect.conn_id,
                 mac_to_str(param->connect.remote_bda).c_str());
        // Botões não precisam de bonding — conexão direta via UUID
        if (!BLETransport::isGateway())
            LedManager::set_node_joined(true);
        break;
    }

    // ── Desconexão → reinicia advertising ───────────────────────────────────
    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "BLE desconectado: conn_id=%d", param->disconnect.conn_id);
        if (!BLETransport::isGateway())
            LedManager::set_node_joined(false);
        s_advertising = false;
        maybe_start_advertising();
        break;

    // ── Write recebido ───────────────────────────────────────────────────────
    case ESP_GATTS_WRITE_EVT:
    {
        uint16_t handle = param->write.handle;
        uint16_t len    = param->write.len;
        const uint8_t *data = param->write.value;

        ESP_LOGD(TAG, "WRITE handle=%d len=%d", handle, len);

        if (len == 0) break;

        if (handle == BLETransport::s_button_char_handle)
        {
            // Pacote de botão — binário (ButtonPacket)
            LedManager::blink(TrafficSource::MESH);
            BLETransport::handle_button_write(data, len);
        }
        else if (handle == BLETransport::s_rx_char_handle)
        {
            // Pacote genérico — JSON Protocol (uso futuro / app)
            std::string json(reinterpret_cast<const char *>(data), len);
            Protocol::Packet packet;
            if (Protocol::parse(json, packet))
            {
                ESP_LOGI(TAG, "BLE RX: %s → %s", packet.route.src.c_str(), packet.route.dst.c_str());
                LedManager::blink(TrafficSource::MESH);
                NetworkManager::handle_incoming(packet);
            }
            else
            {
                ESP_LOGW(TAG, "BLE RX: JSON inválido");
            }
        }
        else
        {
            ESP_LOGD(TAG, "WRITE em handle desconhecido (%d)", handle);
        }
        break;
    }

    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BLETransport::start_gap_gatt
// ─────────────────────────────────────────────────────────────────────────────
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
        // Fallback para BTDM
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

    // Security Manager — botões usam Just Works (sem PIN)
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

// ─────────────────────────────────────────────────────────────────────────────
// BLETransport::start_advertising
// Chamado após o serviço GATT iniciar (ESP_GATTS_START_EVT)
// ─────────────────────────────────────────────────────────────────────────────
void BLETransport::start_advertising()
{
    s_adv_params.adv_int_min       = 0x20; // 20ms — resposta rápida ao botão
    s_adv_params.adv_int_max       = 0x40; // 40ms
    s_adv_params.adv_type          = ADV_TYPE_IND;
    s_adv_params.own_addr_type     = BLE_ADDR_TYPE_PUBLIC;
    s_adv_params.channel_map       = ADV_CHNL_ALL;
    s_adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

    s_adv_data_set = false;
    ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&s_adv_data));
}

// ─────────────────────────────────────────────────────────────────────────────
// BLETransport::node_id
// ─────────────────────────────────────────────────────────────────────────────
std::string BLETransport::node_id()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[32];
    snprintf(buf, sizeof(buf), "node-%02X%02X%02X", mac[3], mac[4], mac[5]);
    return std::string(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// BLETransport::init
// ─────────────────────────────────────────────────────────────────────────────
void BLETransport::init(bool isGateway)
{
    s_isGateway          = isGateway;
    s_service_handle     = 0;
    s_rx_char_handle     = 0;
    s_button_char_handle = 0;

    ESP_LOGI(TAG, "Inicializando BLE (Node) — SERVICE_UUID: 574D0001-AABB-CCDD-8899-102030405060");
    // O restante do setup (create_service → add chars → start_service → advertising)
    // ocorre de forma assíncrona nos handlers GATTS após o app_register.
    start_gap_gatt();

    if (!s_isGateway)
        LedManager::set_node_joined(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// BLETransport::notify_tx / send
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// BLETransport::on_receive_json (uso futuro / app)
// ─────────────────────────────────────────────────────────────────────────────
void BLETransport::on_receive_json(const std::string &jsonString)
{
    Protocol::Packet pkt;
    if (Protocol::parse(jsonString, pkt))
        Router::handle_packet(pkt);
    else
        ESP_LOGW(TAG, "JSON BLE inválido");
}
