#include "ble_transport.hpp"
#include "router.hpp"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"

using namespace WetzelMesh;

static const char *TAG = "BLE";

// UUIDs do serviço/characteristics WetzelMesh
static const uint8_t SERVICE_UUID[16] = {0x57, 0x4D, 0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0x88, 0x99, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60}; // "WM"
static const uint8_t RX_CHAR_UUID[16] = {0x57, 0x4D, 0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x88, 0x99, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
static const uint8_t TX_CHAR_UUID[16] = {0x57, 0x4D, 0x00, 0x03, 0xAA, 0xBB, 0xCC, 0xDD, 0x88, 0x99, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};

static uint16_t g_tx_handle = 0; // notificação
static uint16_t g_rx_handle = 0; // escrita
static esp_gatt_if_t g_gatts_if = ESP_GATT_IF_NONE;

bool BLETransport::s_isGateway = false;

static std::string make_node_name()
{
    // Nome BLE curto (ex: "WM-AB12")
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char buf[16];
    snprintf(buf, sizeof(buf), "WM-%02X%02X", mac[4], mac[5]);
    return std::string(buf);
}

std::string BLETransport::node_id()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char buf[32];
    snprintf(buf, sizeof(buf), "node-%02X%02X%02X", mac[3], mac[4], mac[5]);
    return std::string(buf);
}

static void handle_write_evt(const uint8_t *value, uint16_t len)
{
    std::string json((const char *)value, (size_t)len);
    ESP_LOGI(TAG, "📥 RX JSON via BLE: %.*s", len, (const char *)value);
    BLETransport::on_receive_json(json);
}

// Handlers GATTS muito simplificados
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTS_REG_EVT:
        g_gatts_if = gatts_if;
        esp_ble_gap_set_device_name(make_node_name().c_str());
        esp_ble_gatts_create_attr_tab(nullptr, gatts_if, 0, 0); // usamos API direta abaixo
        break;
    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == g_rx_handle && param->write.len > 0)
            handle_write_evt(param->write.value, param->write.len);
        break;
    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_ADV_START_COMPLETE_EVT)
    {
        ESP_LOGI(TAG, "Advertising iniciado");
    }
}

void BLETransport::start_gap_gatt()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0x55));
}

void BLETransport::setup_service()
{
    // Serviço + duas características: RX (Write) e TX (Notify)
    esp_gatt_srvc_id_t service_id = {};
    service_id.is_primary = true;
    service_id.id.inst_id = 0;
    service_id.id.uuid.len = ESP_UUID_LEN_128;
    memcpy(service_id.id.uuid.uuid.uuid128, SERVICE_UUID, 16);
    ESP_ERROR_CHECK(esp_ble_gatts_create_service(g_gatts_if, &service_id, 8));
    // Simples: registramos handles após CREATE_SERVICE callback normalmente,
    // mas para simplificar, vamos usar valores fixos via create_attr_tab em projetos reais.

    // Aqui, para deixar o arquivo auto contido, vamos assumir que g_service_handle,
    // g_tx_handle e g_rx_handle serão configurados por uma tabela de atributos no seu projeto.
    // ⇒ Se preferir, troque por NimBLE (APIs mais simples) numa próxima iteração.
}

void BLETransport::start_advertising()
{
    esp_ble_adv_params_t adv_params = {};
    adv_params.adv_int_min = 0x40;
    adv_params.adv_int_max = 0x60;
    adv_params.adv_type = ADV_TYPE_IND;
    adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    adv_params.channel_map = ADV_CHNL_ALL;
    adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

    esp_ble_adv_data_t adv_data = {};
    adv_data.set_scan_rsp = false;
    adv_data.include_name = true;
    adv_data.include_txpower = true;

    esp_ble_gap_config_adv_data(&adv_data);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_ble_gap_start_advertising(&adv_params);
}

void BLETransport::init(bool isGateway)
{
    s_isGateway = isGateway;
    ESP_LOGI(TAG, "Inicializando BLE (%s)", isGateway ? "Gateway" : "Node");
    start_gap_gatt();
    setup_service();
    start_advertising();
}

void BLETransport::notify_tx(const std::string &data)
{
    if (g_tx_handle == 0)
    {
        ESP_LOGW(TAG, "TX handle indisponível");
        return;
    }
    esp_ble_gatts_send_indicate(g_gatts_if, 0 /*conn_id*/, g_tx_handle,
                                data.size(), (uint8_t *)data.data(), false);
}

bool BLETransport::send(const Protocol::Packet &packet)
{
    // Para simplificar: enviamos broadcast por notify (num mesh real teríamos bridging entre nós).
    std::string json = Protocol::serialize(packet);
    ESP_LOGI(TAG, "📤 Enviando via BLE (notify): %s", json.c_str());
    notify_tx(json);
    return true;
}

void BLETransport::on_receive_json(const std::string &jsonString)
{
    Protocol::Packet pkt;
    if (Protocol::parse(jsonString, pkt))
    {
        ESP_LOGI(TAG, "Pacote BLE válido — roteando...");
        Router::handle_packet(pkt);
    }
    else
    {
        ESP_LOGW(TAG, "Pacote BLE inválido!");
    }
}
