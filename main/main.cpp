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

#include "uart_bridge.hpp"
#include "interpreter.hpp"
#include "mesh_transport.hpp"

using wetzelmesh::UARTBridge;
using wetzelmesh::Interpreter;
using wetzelmesh::MeshTransport;

static const char *TAG = "NODE";

/* ==========================================================
   HANDLERS DO NODE
   ========================================================== */
static void register_node_handlers(Interpreter &it)
{
    // Handler para ligar o LED (simulado)
    it.registerHandler("led", "on", [](const cJSON *payload) -> cJSON *
                       {
        ESP_LOGI("HANDLER", "LED ON recebido do Gateway");
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "LED ligado");
        return resp; });

    // Handler para desligar o LED
    it.registerHandler("led", "off", [](const cJSON *payload) -> cJSON *
                       {
        ESP_LOGI("HANDLER", "LED OFF recebido do Gateway");
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "LED desligado");
        return resp; });

    ESP_LOGI(TAG, "Handlers do Node registrados");
}

/* ==========================================================
   FUNÇÃO PRINCIPAL
   ========================================================== */
extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "WetzelMesh Node inicializando...");

    // Inicializa o transporte UART
    static MeshTransport transport;
    transport.init();

    // Cria o intérprete
    static Interpreter interpreter;
    register_node_handlers(interpreter);

    // Define o handler de mensagens recebidas do gateway
    transport.setRxHandler([&](const std::string &json)
                           {
        std::string response;
        interpreter.handleMessage(json, response);
        if (!response.empty()) {
            transport.send(response);
        } });

    // Task para envio de telemetria periódica
    xTaskCreate([](void *arg)
                {
        MeshTransport* t = static_cast<MeshTransport*>(arg);
        int counter = 0;

        while (true) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "type", "command");
            cJSON_AddStringToObject(root, "target", "telemetry");
            cJSON_AddStringToObject(root, "action", "push");
            cJSON_AddNumberToObject(root, "id", counter++);

            cJSON* payload = cJSON_CreateObject();
            cJSON_AddNumberToObject(payload, "temperature", 22.0 + (rand() % 100) / 10.0);
            cJSON_AddNumberToObject(payload, "humidity", 55.0 + (rand() % 200) / 10.0);
            cJSON_AddNumberToObject(payload, "battery", 3.7 + (rand() % 20) / 100.0);
            cJSON_AddItemToObject(root, "payload", payload);

            char* s = cJSON_PrintUnformatted(root);
            std::string out = s ? s : "{}";
            if (s) cJSON_free(s);
            cJSON_Delete(root);

            ESP_LOGI("TX_TASK", "Enviando telemetria...");
            t->send(out);

            vTaskDelay(pdMS_TO_TICKS(5000)); // a cada 5 segundos
        } }, "telemetry_task", 4096, &transport, 5, nullptr);

    ESP_LOGI(TAG, "Node pronto e transmitindo dados");
}
