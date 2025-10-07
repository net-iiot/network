#include <string>
#include <vector>

extern "C"
{
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

#include "uart_bridge.hpp" // seu componente (namespace que você já usa)
#include "interpreter.hpp" // novo componente (wetzelmesh::Interpreter)

#ifndef CONFIG_LOG_MAXIMUM_LEVEL
#define CONFIG_LOG_MAXIMUM_LEVEL 3
#endif

using wetzelmesh::UARTBridge;

static const char *TAG = "APP";

static void register_default_handlers(wetzelmesh::Interpreter &it)
{
    using wetzelmesh::Interpreter;
    // Handler LED ON (simulado)
    it.registerHandler("led", "on", [](const cJSON *payload) -> cJSON *
                       {
                           // Aqui você poderia acionar um GPIO real. Vamos só simular.
                           bool ok = true;
                           ESP_LOGI("HANDLER", "LED -> ON (simulado)");
                           cJSON *data = cJSON_CreateObject();
                           cJSON_AddStringToObject(data, "result", ok ? "turned_on" : "failed");
                           return data; // ownership transferido
                       });

    // Handler LED OFF (simulado)
    it.registerHandler("led", "off", [](const cJSON *payload) -> cJSON *
                       {
        bool ok = true;
        ESP_LOGI("HANDLER", "LED -> OFF (simulado)");
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "result", ok ? "turned_off" : "failed");
        return data; });

    // Handler echo (retorna o payload de volta)
    it.registerHandler("sys", "echo", [](const cJSON *payload) -> cJSON *
                       {
        cJSON* data = cJSON_CreateObject();
        if (payload) {
            // clona o payload para dentro de "echo"
            cJSON_AddItemToObject(data, "echo", cJSON_Duplicate(payload, /*recurse=*/1));
        } else {
            cJSON_AddStringToObject(data, "echo", "null");
        }
        return data; });
}

extern "C" void app_main(void)
{
    // NVS (requerido por Wi-Fi/BLE em geral; seguro manter)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "wetzelmesh boot OK (C++ project)");

    // Inicializa UARTBridge (pinos 13=TX, 15=RX estão no componente)
    UARTBridge uart;
    uart.init();

    // Cria e registra protocolo
    static wetzelmesh::Interpreter proto;
    register_default_handlers(proto);

    // RX: tudo que chegar na UART (JSON) é passado ao interprete; resposta volta pela UART.
    uart.setPacketHandler([&](const std::vector<uint8_t> &pkt)
                          {
        std::string in(pkt.begin(), pkt.end());
        std::string out;
        bool ok = proto.handleMessage(in, out);
        ESP_LOGI("ROUTER", "RX JSON: %s", in.c_str());
        ESP_LOGI("ROUTER", "TX JSON: %s", out.c_str());
        // responde ao remetente (via UART mesmo)
        std::vector<uint8_t> resp(out.begin(), out.end());
        uart.sendPacket(resp);
        (void)ok; });

    // TX: tarefa de teste que envia comando echo a cada 3s
    xTaskCreate([](void *arg)
                {
        UARTBridge* u = static_cast<UARTBridge*>(arg);
        int seq = 0;
        while (true) {
            // Monta um JSON mínimo aceito pela camada:
            // {"type":"command","target":"sys","action":"echo","id":<seq>,"payload":{"msg":"hello"}}
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "type", "command");
            cJSON_AddStringToObject(root, "target", "sys");
            cJSON_AddStringToObject(root, "action", "echo");
            cJSON_AddNumberToObject(root, "id", seq++);
            cJSON* pl = cJSON_CreateObject();
            cJSON_AddStringToObject(pl, "msg", "hello from wetzelmesh");
            cJSON_AddItemToObject(root, "payload", pl);

            char* s = cJSON_PrintUnformatted(root);
            std::string out = s ? s : "{}";
            if (s) cJSON_free(s);
            cJSON_Delete(root);

            std::vector<uint8_t> bytes(out.begin(), out.end());
            u->sendPacket(bytes);

            vTaskDelay(pdMS_TO_TICKS(3000));
        } }, "tx_test", 4096, &uart, 5, nullptr);

    ESP_LOGI(TAG, "APP: System initialized");
}
