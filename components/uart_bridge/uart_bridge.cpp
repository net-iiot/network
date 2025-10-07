#include "uart_bridge.hpp"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstring>

namespace wetzelmesh
{
    static const char *TAG = "UARTBridge";

#define UART_PORT UART_NUM_2
#define UART_TX_PIN 13
#define UART_RX_PIN 15
#define UART_BAUD 115200
#define UART_BUF_SIZE 2048

#define FRAME_HEADER 0xAA55
#define FRAME_FOOTER 0x55AA

    static QueueHandle_t uartQueue;

    esp_err_t UARTBridge::init()
    {
        ESP_LOGI(TAG, "UART init TX=%d RX=%d", UART_TX_PIN, UART_RX_PIN);

        uart_config_t config = {
            .baud_rate = UART_BAUD,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 0,
            .flags = 0};

        ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 10, &uartQueue, 0));
        ESP_ERROR_CHECK(uart_param_config(UART_PORT, &config));
        ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

        xTaskCreate(rxTask, "uart_rx_task", 4096, this, 10, nullptr);
        return ESP_OK;
    }

    void UARTBridge::setPacketHandler(PacketHandler handler)
    {
        onPacket = std::move(handler);
    }

    esp_err_t UARTBridge::sendPacket(const std::vector<uint8_t> &data)
    {
        uint16_t len = data.size();
        uint32_t crc = crc32(data.data(), len);
        uint8_t frame[8 + len]; // header(2)+len(2)+data+crc(4)
        uint16_t header = FRAME_HEADER;
        uint16_t footer = FRAME_FOOTER;

        size_t pos = 0;
        memcpy(&frame[pos], &header, 2);
        pos += 2;
        memcpy(&frame[pos], &len, 2);
        pos += 2;
        memcpy(&frame[pos], data.data(), len);
        pos += len;
        memcpy(&frame[pos], &crc, 4);
        pos += 4;
        memcpy(&frame[pos], &footer, 2);
        pos += 2;

        int written = uart_write_bytes(UART_PORT, (const char *)frame, pos);
        ESP_LOGD(TAG, "Sent frame len=%d (payload=%d)", written, len);
        return written > 0 ? ESP_OK : ESP_FAIL;
    }

    void UARTBridge::rxTask(void *arg)
    {
        auto *self = static_cast<UARTBridge *>(arg);
        uint8_t buffer[UART_BUF_SIZE];
        std::vector<uint8_t> rx;
        rx.reserve(UART_BUF_SIZE);

        while (true)
        {
            int len = uart_read_bytes(UART_PORT, buffer, sizeof(buffer), pdMS_TO_TICKS(100));
            if (len > 0)
            {
                self->handleIncoming(buffer, len);
            }
        }
    }

    void UARTBridge::handleIncoming(const uint8_t *data, size_t len)
    {
        static std::vector<uint8_t> stream;
        stream.insert(stream.end(), data, data + len);

        while (stream.size() >= 10)
        { // header + len + footer mínimo
            uint16_t header;
            memcpy(&header, stream.data(), 2);
            if (header != FRAME_HEADER)
            {
                stream.erase(stream.begin()); // procura header
                continue;
            }

            if (stream.size() < 4)
                return;
            uint16_t payloadLen;
            memcpy(&payloadLen, stream.data() + 2, 2);
            size_t totalLen = 2 + 2 + payloadLen + 4 + 2;

            if (stream.size() < totalLen)
                return; // ainda não chegou tudo

            uint32_t crc;
            memcpy(&crc, stream.data() + 4 + payloadLen, 4);

            uint16_t footer;
            memcpy(&footer, stream.data() + 8 + payloadLen - 2, 2);

            if (footer != FRAME_FOOTER)
            {
                ESP_LOGW(TAG, "Footer mismatch, discarding packet");
                stream.erase(stream.begin(), stream.begin() + totalLen);
                continue;
            }

            uint32_t calc = crc32(stream.data() + 4, payloadLen);
            if (calc != crc)
            {
                ESP_LOGW(TAG, "CRC mismatch");
            }
            else if (onPacket)
            {
                std::vector<uint8_t> payload(stream.begin() + 4, stream.begin() + 4 + payloadLen);
                onPacket(payload);
            }

            stream.erase(stream.begin(), stream.begin() + totalLen);
        }
    }

    uint32_t UARTBridge::crc32(const uint8_t *data, size_t len)
    {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; i++)
        {
            crc ^= data[i];
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
        return ~crc;
    }

}
