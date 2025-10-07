#include "gateway.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "router.hpp"

using namespace WetzelMesh;
static const char *TAG = "GATEWAY";

void Gateway::init()
{
    ESP_LOGI(TAG, "Inicializando UART do gateway...");

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT};

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Cria task para receber dados
    xTaskCreate(listen_task, "gateway_uart_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Gateway UART inicializado (TX=%d, RX=%d)", TX_PIN, RX_PIN);
}

void Gateway::send(const Protocol::Packet &packet)
{
    std::string json = Protocol::serialize(packet);
    uart_write_bytes(UART_PORT, json.c_str(), json.length());
    uart_write_bytes(UART_PORT, "\n", 1); // separador de pacotes
    ESP_LOGI(TAG, "→ Enviado via UART: %s", json.c_str());
}

void Gateway::listen_task(void *param)
{
    uint8_t data[BUF_SIZE];
    while (true)
    {
        int len = uart_read_bytes(UART_PORT, data, BUF_SIZE - 1, pdMS_TO_TICKS(200));
        if (len > 0)
        {
            data[len] = '\0';
            std::string json(reinterpret_cast<char *>(data));
            ESP_LOGI(TAG, "← Recebido via UART: %s", json.c_str());

            Protocol::Packet packet;
            if (Protocol::parse(json, packet))
            {
                ESP_LOGI(TAG, "Pacote UART válido recebido. Encaminhando ao Router...");
                Router::handle_packet(packet);
            }
            else
            {
                ESP_LOGW(TAG, "Falha ao interpretar pacote recebido via UART.");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
