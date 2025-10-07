#include "ble_transport.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "nvs_flash.h"
#include "router.hpp"

using namespace WetzelMesh;
static const char *TAG = "BLE";

// UUID único da rede (invisível a apps normais)
static const uint8_t WETZELMESH_UUID[16] = {
    0x57, 0x45, 0x54, 0x5A, 0x45, 0x4C, 0x4D, 0x45, 0x53, 0x48, 0x00, 0x00, 0x00, 0x10, 0x20, 0x25
};

void BLETransport::init()
{
    ESP_LOGI(TAG, "Inicializando BLE Transport...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init();
    esp_bluedroid_enable();

    advertise();

    ESP_LOGI(TAG, "BLE Transport inicializado. UUID de rede: WETZELMESH");
}

void BLETransport::advertise()
{
    ESP_LOGI(TAG, "Iniciando publicidade BLE (invisível a dispositivos comuns)...");

    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = false,
        .include_txpower = false,
        .min_interval = 0x20,
        .max_interval = 0x40,
        .appearance = 0x00,
        .manufacturer_len = sizeof(WETZELMESH_UUID),
        .p_manufacturer_data = (uint8_t *)WETZELMESH_UUID,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = 0,
        .p_service_uuid = NULL,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT)
    };

    esp_ble_gap_config_adv_data(&adv_data);
}

void BLETransport::send(const Protocol::Packet &packet)
{
    std::string json = Protocol::serialize(packet);
    ESP_LOGI(TAG, "📡 Enviando via BLE: %s", json.c_str());
    // Aqui entra o envio real BLE Mesh no futuro (esp_ble_gatts_send_indicate)
}

void BLETransport::on_receive(const std::string &data)
{
    ESP_LOGI(TAG, "📥 Recebido via BLE: %s", data.c_str());

    Protocol::Packet pkt;
    if (Protocol::parse(data, pkt)) {
        ESP_LOGI(TAG, "Pacote BLE válido — roteando...");
        Router::handle_packet(pkt);
    } else {
        ESP_LOGW(TAG, "Pacote BLE inválido!");
    }
}
