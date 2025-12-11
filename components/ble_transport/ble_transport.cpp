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

// -----------------------------------------------------------------------------
// UUIDs do serviço/characteristics WetzelMesh (placeholder)
// -----------------------------------------------------------------------------
static const uint8_t SERVICE_UUID[16] = {0x57, 0x4D, 0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0x88, 0x99, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60}; // "WM"
static const uint8_t RX_CHAR_UUID[16] = {0x57, 0x4D, 0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x88, 0x99, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
static const uint8_t TX_CHAR_UUID[16] = {0x57, 0x4D, 0x00, 0x03, 0xAA, 0xBB, 0xCC, 0xDD, 0x88, 0x99, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};

// -----------------------------------------------------------------------------
// Handles / flags
// -----------------------------------------------------------------------------
static uint16_t g_tx_handle = 0; // notificação
static esp_gatt_if_t g_gatts_if = ESP_GATT_IF_NONE;
bool BLETransport::s_isGateway = false;

// -----------------------------------------------------------------------------
// Bonding/Pairing - dispositivos pareados
// -----------------------------------------------------------------------------
static std::set<std::string> s_bonded_devices; // MAC addresses dos dispositivos pareados
static const char* BONDED_DEVICES_NVS_NAMESPACE = "ble_bonded";
static const char* BONDED_DEVICES_KEY = "devices";

// -----------------------------------------------------------------------------
// Advertising global
// -----------------------------------------------------------------------------
static esp_ble_adv_params_t s_adv_params = {};
static bool s_adv_data_set = false;
static bool s_scan_rsp_set = false; // <- não usamos scan_rsp por padrão
static bool s_advertising = false;

// -----------------------------------------------------------------------------
// Advertising data (enxuto): nome + UUID de serviço
// -----------------------------------------------------------------------------
static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = false, // Será configurado dinamicamente baseado na visibilidade
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len = 0,
    .p_service_data = nullptr,
    .service_uuid_len = sizeof(SERVICE_UUID),
    .p_service_uuid = (uint8_t *)SERVICE_UUID,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT)};

// -----------------------------------------------------------------------------
// Funções auxiliares
// -----------------------------------------------------------------------------
static std::string get_ble_network_name()
{
#ifdef CONFIG_WETZEL_BLE_NETWORK_NAME
    return std::string(CONFIG_WETZEL_BLE_NETWORK_NAME);
#else
    return std::string("WetzelMesh"); // Fallback
#endif
}

std::string BLETransport::node_id()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[32];
    snprintf(buf, sizeof(buf), "node-%02X%02X%02X", mac[3], mac[4], mac[5]);
    return std::string(buf);
}

// -----------------------------------------------------------------------------
// Advertising helper
// -----------------------------------------------------------------------------
static void maybe_start_advertising()
{
    if (!s_advertising && s_adv_data_set)
    {
        esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
        if (err == ESP_OK)
        {
            s_advertising = true;
            ESP_LOGI(TAG, "Advertising iniciado");
        }
        else
        {
            ESP_LOGE(TAG, "Falha ao iniciar advertising: %s", esp_err_to_name(err));
        }
    }
}

// -----------------------------------------------------------------------------
// Funções de gerenciamento de bonding
// -----------------------------------------------------------------------------
static std::string mac_to_string(const uint8_t* mac)
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

static void save_bonded_devices()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(BONDED_DEVICES_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Erro ao abrir NVS para salvar dispositivos pareados: %s", esp_err_to_name(err));
        return;
    }

    // Serializa lista de MAC addresses (um por linha)
    std::string devices_list;
    for (const auto& mac : s_bonded_devices)
    {
        devices_list += mac + "\n";
    }

    err = nvs_set_str(nvs_handle, BONDED_DEVICES_KEY, devices_list.c_str());
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Erro ao salvar dispositivos pareados: %s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Dispositivos pareados salvos (%zu dispositivos)", s_bonded_devices.size());
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static void load_bonded_devices()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(BONDED_DEVICES_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "Nenhum dispositivo pareado encontrado (primeira execução)");
        return;
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, BONDED_DEVICES_KEY, nullptr, &required_size);
    if (err != ESP_OK || required_size == 0)
    {
        ESP_LOGI(TAG, "Nenhum dispositivo pareado salvo");
        nvs_close(nvs_handle);
        return;
    }

    std::vector<char> buffer(required_size);
    err = nvs_get_str(nvs_handle, BONDED_DEVICES_KEY, buffer.data(), &required_size);
    if (err == ESP_OK)
    {
        std::string devices_list(buffer.data());
        std::string mac;
        for (char c : devices_list)
        {
            if (c == '\n' || c == '\r')
            {
                if (!mac.empty())
                {
                    s_bonded_devices.insert(mac);
                    ESP_LOGI(TAG, "Dispositivo pareado carregado: %s", mac.c_str());
                    mac.clear();
                }
            }
            else
            {
                mac += c;
            }
        }
        if (!mac.empty())
        {
            s_bonded_devices.insert(mac);
            ESP_LOGI(TAG, "Dispositivo pareado carregado: %s", mac.c_str());
        }
        ESP_LOGI(TAG, "Total de dispositivos pareados carregados: %zu", s_bonded_devices.size());
    }

    nvs_close(nvs_handle);
}

static void add_bonded_device(const uint8_t* mac)
{
    std::string mac_str = mac_to_string(mac);
    if (s_bonded_devices.find(mac_str) == s_bonded_devices.end())
    {
        s_bonded_devices.insert(mac_str);
        ESP_LOGI(TAG, "Novo dispositivo pareado adicionado: %s", mac_str.c_str());
        save_bonded_devices();
    }
}

static bool is_bonded_device(const uint8_t* mac)
{
    std::string mac_str = mac_to_string(mac);
    return s_bonded_devices.find(mac_str) != s_bonded_devices.end();
}


// -----------------------------------------------------------------------------
// GAP handler
// -----------------------------------------------------------------------------
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_data_set = true;
        ESP_LOGD(TAG, "ADV data configurado");
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
        ESP_LOGI(TAG, "Advertising parado");
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "Solicitação de segurança BLE recebida");
        // Aceita solicitação de segurança (bonding)
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success)
        {
            ESP_LOGI(TAG, "Autenticação BLE concluída com sucesso");
            add_bonded_device(param->ble_security.auth_cmpl.bd_addr);
        }
        else
        {
            ESP_LOGW(TAG, "Autenticação BLE falhou");
        }
        break;

    case ESP_GAP_BLE_KEY_EVT:
        ESP_LOGI(TAG, "Chave BLE recebida (bonding em progresso)");
        break;

    default:
        break;
    }
}

// -----------------------------------------------------------------------------
// GATTS handler (mínimo; sem tabela de atributos por enquanto)
// -----------------------------------------------------------------------------
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTS_REG_EVT:
        g_gatts_if = gatts_if;
        ESP_LOGI(TAG, "GATT registrado (app_id=0x%x)", param->reg.app_id);
        {
            const std::string name = get_ble_network_name();
            ESP_LOGI(TAG, "Configurando nome BLE: '%s'", name.c_str());
            ESP_ERROR_CHECK(esp_ble_gap_set_device_name(name.c_str()));
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
    {
        const uint8_t* remote_mac = param->connect.remote_bda;
        std::string mac_str = mac_to_string(remote_mac);
        ESP_LOGI(TAG, "BLE conectado: conn_id=%d, MAC=%s", param->connect.conn_id, mac_str.c_str());
        
        // Verifica se é um dispositivo já pareado
        if (is_bonded_device(remote_mac))
        {
            ESP_LOGI(TAG, "Dispositivo pareado reconectado automaticamente: %s", mac_str.c_str());
        }
        else
        {
            ESP_LOGI(TAG, "Novo dispositivo conectado (será pareado): %s", mac_str.c_str());
            // Inicia processo de bonding (faz cast de const para não-const)
            uint8_t mac_copy[6];
            memcpy(mac_copy, remote_mac, 6);
            esp_ble_set_encryption(mac_copy, ESP_BLE_SEC_ENCRYPT);
        }
        
        if (!BLETransport::isGateway())
            LedManager::set_node_joined(true);
        break;
    }

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGW(TAG, "BLE desconectado: conn_id=%d", param->disconnect.conn_id);
        if (!BLETransport::isGateway())
            LedManager::set_node_joined(false);
        s_advertising = false;
        maybe_start_advertising();
        break;

    case ESP_GATTS_WRITE_EVT:
        ESP_LOGD(TAG, "GATT WRITE recebido, len=%d", param->write.len);
        if (param->write.len > 0)
        {
            std::string json(reinterpret_cast<char *>(param->write.value), param->write.len);
            Protocol::Packet packet;
            if (Protocol::parse(json, packet))
            {
                ESP_LOGI(TAG, "BLE pacote RX: %s -> %s",
                         packet.route.src.c_str(), packet.route.dst.c_str());
                LedManager::blink(TrafficSource::MESH); // ATUALIZADO (tráfego Mesh/BLE)
                NetworkManager::handle_incoming(packet);
            }
        }
        break;

    default:
        break;
    }
}

// -----------------------------------------------------------------------------
// BLETransport: inicialização
// -----------------------------------------------------------------------------
void BLETransport::start_gap_gatt()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = ESP_BT_MODE_BLE;

    ESP_LOGI(TAG, "Inicializando controlador BLE...");
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "Falha ao liberar Classic BT: %s", esp_err_to_name(err));

    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(err);

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err == ESP_ERR_INVALID_ARG)
    {
        ESP_LOGW(TAG, "Fallback para BTDM...");
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

    // Configura Security Manager para permitir bonding
    ESP_LOGI(TAG, "Configurando Security Manager (bonding habilitado)...");
    
    // IO Capabilities (Just Works - sem entrada de PIN)
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    
    // Authentication requirements (Security + MITM + Bonding)
    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    
    // Tamanho máximo da chave de criptografia
    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    
    // Chaves iniciais (encryption + identity)
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    
    // Chaves de resposta (encryption + identity)
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    // Carrega dispositivos pareados salvos
    load_bonded_devices();

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0x55));
    
    ESP_LOGI(TAG, "Security Manager configurado - bonding habilitado");
}

void BLETransport::setup_service()
{
    // Mantemos um service mínimo (sem attr table por enquanto)
    esp_gatt_srvc_id_t service_id = {};
    service_id.is_primary = true;
    service_id.id.inst_id = 0;
    service_id.id.uuid.len = ESP_UUID_LEN_128;
    memcpy(service_id.id.uuid.uuid.uuid128, SERVICE_UUID, 16);
    ESP_ERROR_CHECK(esp_ble_gatts_create_service(g_gatts_if, &service_id, 8));
}

void BLETransport::start_advertising()
{
    s_adv_params.adv_int_min = 0x40;
    s_adv_params.adv_int_max = 0x60;
    
    // Sempre usa ADV_TYPE_IND (connectable) - permite conexões mesmo se invisível
    // A diferença entre visível/invisível é apenas se o nome aparece no advertising
    s_adv_params.adv_type = ADV_TYPE_IND; // Connectable undirected advertising
    s_adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    s_adv_params.channel_map = ADV_CHNL_ALL;
    s_adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

    s_adv_data_set = false;
    s_scan_rsp_set = false; // não vamos usar scan response

    // Configura se o nome será incluído no advertising (apenas se visível)
#ifdef CONFIG_WETZEL_BLE_VISIBLE
    s_adv_data.include_name = true;
    ESP_LOGI(TAG, "BLE configurado como VISÍVEL (aparece em scans com nome)");
#else
    s_adv_data.include_name = false; // Não inclui nome se invisível (mas ainda conectável via UUID)
    ESP_LOGI(TAG, "BLE configurado como INVISÍVEL (não aparece em scans, mas conectável via UUID)");
#endif
    
    s_adv_data.manufacturer_len = 0;
    s_adv_data.p_manufacturer_data = nullptr;

    ESP_LOGI(TAG, "Configurando advertising BLE (nome incluído: %s, tipo: connectable)", 
             s_adv_data.include_name ? "SIM" : "NÃO");
    ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&s_adv_data));
}

void BLETransport::init(bool isGateway)
{
    s_isGateway = isGateway;
    ESP_LOGI(TAG, "Inicializando BLE (%s)", isGateway ? "Gateway" : "Node");
    start_gap_gatt();
    setup_service();
    start_advertising();
    if (!s_isGateway)
        LedManager::set_node_joined(false);
}

void BLETransport::notify_tx(const std::string &data)
{
    if (g_tx_handle == 0)
    {
        ESP_LOGW(TAG, "TX handle indisponível");
        return;
    }
    esp_ble_gatts_send_indicate(g_gatts_if, 0, g_tx_handle,
                                data.size(), (uint8_t *)data.data(), false);
}

bool BLETransport::send(const Protocol::Packet &packet)
{
    std::string json = Protocol::serialize(packet);
    ESP_LOGI(TAG, "Enviando via BLE (notify): %s", json.c_str());
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
