#include "gateway.hpp"
#include "led_manager.hpp"
#include "protocol.hpp"
#include "router.hpp"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_mac.h"

using namespace WetzelMesh;

static const char *TAG = "GATEWAY";

void Gateway::init()
{
    ESP_LOGI(TAG, "Inicializando Gateway...");

    // LED: começa "desconectado"
    LedManager::set_gateway_server_connected(false);

    // Config UART conforme header (TX_PIN=13, RX_PIN=15)
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

    // Cria a task usando o MÉTODO estático da classe
    xTaskCreate(&Gateway::listen_task, "gateway_uart_listener", 4096, nullptr, 5, nullptr);

    ESP_LOGI(TAG, "Gateway inicializado — aguardando servidor via UART...");
}

// ⚠️ Agora é o MÉTODO da classe, não uma função livre
void Gateway::listen_task(void *param)
{
    uint8_t buf[BUF_SIZE];
    std::string acc;

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

                Protocol::Packet packet;
                if (Protocol::parse(line, packet))
                {
                    ESP_LOGI(TAG, "📥 UART <- servidor: %s", line.c_str());
                    static bool s_markedConnected = false;
                    if (!s_markedConnected)
                    {
                        LedManager::set_gateway_server_connected(true);
                        s_markedConnected = true;
                        ESP_LOGI(TAG, "🔌 Gateway conectado ao servidor — LED atualizado");
                    }
                    Router::handle_packet(packet);
                }
                else
                {
                    ESP_LOGW(TAG, "Linha UART inválida: %s", line.c_str());
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool Gateway::send(const Protocol::Packet &packet)
{
    std::string json = Protocol::serialize(packet);
    json.push_back('\n');

    const uart_port_t uart_num = UART_PORT;
    int written = uart_write_bytes(uart_num, json.c_str(), json.size());
    if (written < 0)
    {
        ESP_LOGE("GATEWAY", "Falha ao enviar pacote via UART");
        return false;
    }

    ESP_LOGI("GATEWAY", "📤 Enviado via UART: %s", json.c_str());
    LedManager::on_packet_received();
    return true;
}
