#include "gateway.hpp"
#include "protocol.hpp" // <--- aqui sim!
#include "led_manager.hpp"
#include "router.hpp"

#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_client.h" // <--- precisa do REQUIRES correto no CMake

namespace WetzelMesh
{

    static const char *TAG = "GATEWAY";

    // --- Wi-Fi STA ---
    void Gateway::init_wifi(const char *ssid, const char *pass)
    {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        wifi_config_t sta_cfg = {};
        // cuidado com o tamanho/terminador:
        strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_connect());

        ESP_LOGI(TAG, "Wi-Fi STA conectando em SSID=%s ...", ssid);
    }

    // --- HTTP ---
    static esp_err_t _http_event(esp_http_client_event_t *) { return ESP_OK; }

    bool Gateway::http_post_json(const std::string &url, const std::string &body, std::string &out_resp)
    {
        esp_http_client_config_t cfg = {};
        cfg.url = url.c_str();
        cfg.method = HTTP_METHOD_POST;
        cfg.event_handler = _http_event;
        cfg.timeout_ms = 5000;

        esp_http_client_handle_t h = esp_http_client_init(&cfg);
        if (!h)
            return false;

        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_post_field(h, body.c_str(), body.size());

        esp_err_t err = esp_http_client_perform(h);
        if (err != ESP_OK)
        {
            esp_http_client_cleanup(h);
            return false;
        }

        int status = esp_http_client_get_status_code(h);
        int len = esp_http_client_get_content_length(h);

        out_resp.clear();
        if (status == 200 && len > 0)
        {
            out_resp.resize(len);
            int r = esp_http_client_read_response(h, out_resp.data(), len);
            if (r < 0)
                out_resp.clear();
        }
        esp_http_client_cleanup(h);
        return status == 200;
    }

    // --- Inicialização principal do Gateway ---
    void Gateway::init()
    {
        ESP_LOGI(TAG, "Inicializando Gateway (Wi-Fi + HTTP + UART)...");
        LedManager::set_gateway_server_connected(false);

        // NVS (se não foi inicializado no main)
        // ESP_ERROR_CHECK(nvs_flash_init());

        // 1) Wi-Fi STA
        init_wifi("SSID_EMPRESA", "SENHA_FORTE");

        // 2) UART (ponte com o node-edge)
        uart_config_t cfg = {};
        cfg.baud_rate = BAUD;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.rx_flow_ctrl_thresh = 122;
        cfg.source_clk = UART_SCLK_APB;

        ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE, BUF_SIZE, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
        ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

        xTaskCreate(&Gateway::listen_task, "gateway_uart_listener", 4096, nullptr, 5, nullptr);
        ESP_LOGI(TAG, "Gateway pronto — aguardando UART para repassar ao servidor.");
    }

    // --- Task: UART RX -> HTTP POST -> UART TX (resposta) ---
    void Gateway::listen_task(void *param)
    {
        uint8_t buf[BUF_SIZE];
        std::string acc;
        bool server_marked = false;

        while (true)
        {
            int len = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
            if (len > 0)
            {
                acc.append(reinterpret_cast<const char *>(buf), len);

                size_t pos;
                while ((pos = acc.find('\n')) != std::string::npos)
                {
                    std::string line = acc.substr(0, pos);
                    acc.erase(0, pos + 1);

                    Protocol::Packet pkt;
                    if (!Protocol::parse(line, pkt))
                    {
                        ESP_LOGW(TAG, "UART linha inválida: %s", line.c_str());
                        continue;
                    }

                    if (!server_marked)
                    {
                        LedManager::set_gateway_server_connected(true); // LED: conectado ao server
                        server_marked = true;
                    }

                    // envia ao server via HTTP
                    std::string body = Protocol::serialize(pkt);
                    std::string resp;
                    bool ok = http_post_json("http://10.0.0.10:8080/api/mesh", body, resp);

                    // devolve resposta (se houver) ao node-edge pela UART
                    if (ok && !resp.empty())
                    {
                        resp.push_back('\n');
                        uart_write_bytes(UART_PORT, resp.c_str(), resp.size());
                    }

                    LedManager::on_packet_received(); // pisca no tráfego
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // --- Envio direto para o node-edge (UART TX) ---
    void Gateway::send(const Protocol::Packet &packet)
    {
        std::string json = Protocol::serialize(packet);
        json.push_back('\n');
        uart_write_bytes(UART_PORT, json.c_str(), json.size());
        LedManager::on_packet_received();
    }

} // namespace WetzelMesh
