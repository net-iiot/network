#include "ota_manager.hpp"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "network_manager.hpp"
#include "gateway.hpp"
#include "protocol.hpp"
#include <cstring>
#include <sstream>

namespace WetzelMesh
{
    static const char *TAG = "OTA";

    bool OTAManager::s_isGateway = false;
    std::string OTAManager::s_server_url = "";
    OTAStatus OTAManager::s_status = OTAStatus::IDLE;
    int OTAManager::s_download_progress = 0;
    FirmwareVersion OTAManager::s_current_version;
    OTAManager::StatusCallback OTAManager::s_status_callback = nullptr;

    std::string OTAManager::get_version_string()
    {
        const esp_app_desc_t *app_desc = esp_app_get_description();
        std::ostringstream oss;
        oss << app_desc->version << "-" << app_desc->project_name;
        return oss.str();
    }

    FirmwareVersion OTAManager::get_current_version()
    {
        const esp_app_desc_t *app_desc = esp_app_get_description();
        s_current_version.version = app_desc->version;
        s_current_version.build_date = app_desc->date;
        
        // idf_ver é uma string, não uma estrutura
        // Usa um hash simples da string como build_number
        std::string idf_ver_str = app_desc->idf_ver;
        uint32_t hash = 0;
        for (char c : idf_ver_str) {
            hash = hash * 31 + c;
        }
        s_current_version.build_number = hash;
        
        // Calcula hash simples do firmware
        std::ostringstream oss;
        oss << app_desc->version << app_desc->date << app_desc->time;
        s_current_version.hash = oss.str();
        
        return s_current_version;
    }

    // Wrapper C para a task (xTaskCreate precisa de função C, não método C++)
    static void ota_task_wrapper(void *param)
    {
        OTAManager::ota_task(param);
    }

    void OTAManager::init(bool isGateway, const std::string &server_url)
    {
        s_isGateway = isGateway;
        s_server_url = server_url;
        s_status = OTAStatus::IDLE;
        s_download_progress = 0;
        
        get_current_version();
        
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "OTA Manager inicializado");
        ESP_LOGI(TAG, "Versão atual: %s", s_current_version.version.c_str());
        ESP_LOGI(TAG, "Build date: %s", s_current_version.build_date.c_str());
        ESP_LOGI(TAG, "Modo: %s", isGateway ? "Gateway" : "Node");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        
        if (isGateway)
        {
            // Gateway: cria task para verificar atualizações periodicamente
            xTaskCreatePinnedToCore(ota_task_wrapper, "ota_task", 8192, nullptr, 5, nullptr, tskNO_AFFINITY);
        }
    }

    void OTAManager::set_status_callback(StatusCallback cb)
    {
        s_status_callback = cb;
    }

    OTAStatus OTAManager::get_status()
    {
        return s_status;
    }

    int OTAManager::get_download_progress()
    {
        return s_download_progress;
    }

    FirmwareVersion OTAManager::parse_version_info(const std::string &json)
    {
        FirmwareVersion version;
        // Parse simples do JSON (pode ser melhorado)
        // Formato esperado: {"version":"1.0.1","build_date":"2025-01-15","build_number":1001,"hash":"abc123"}
        // Por enquanto, retorna versão vazia
        return version;
    }

    void OTAManager::check_for_update()
    {
        if (s_status != OTAStatus::IDLE)
        {
            ESP_LOGW(TAG, "OTA já está em andamento, ignorando check_for_update");
            return;
        }

        if (s_server_url.empty())
        {
            ESP_LOGW(TAG, "Server URL não configurado, não é possível verificar atualizações");
            return;
        }

        ESP_LOGI(TAG, "Verificando atualizações disponíveis...");
        s_status = OTAStatus::CHECKING;
        
        if (s_status_callback)
            s_status_callback(s_status, "Verificando atualizações...");

        // Cria requisição HTTP GET para verificar versão
        std::string version_url = s_server_url + "/api/firmware/version";
        
        esp_http_client_config_t config = {};
        config.url = version_url.c_str();
        config.timeout_ms = 5000;
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client)
        {
            ESP_LOGE(TAG, "Falha ao criar cliente HTTP");
            s_status = OTAStatus::FAILED;
            if (s_status_callback)
                s_status_callback(s_status, "Falha ao criar cliente HTTP");
            return;
        }

        // Define método GET explicitamente
        esp_http_client_set_method(client, HTTP_METHOD_GET);
        
        ESP_LOGI(TAG, "Fazendo GET para: %s", version_url.c_str());
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK)
        {
            int status_code = esp_http_client_get_status_code(client);
            int content_length = esp_http_client_get_content_length(client);
            
            if (status_code == 200 && content_length > 0)
            {
                char *buffer = (char *)malloc(content_length + 1);
                if (buffer)
                {
                    int data_read = esp_http_client_read(client, buffer, content_length);
                    if (data_read > 0)
                    {
                        buffer[data_read] = '\0';
                        ESP_LOGI(TAG, "Resposta do servidor: %s", buffer);
                        
                        // Parse da resposta e comparação de versão
                        // Por enquanto, apenas loga
                    }
                    free(buffer);
                }
            }
            else
            {
                ESP_LOGI(TAG, "Nenhuma atualização disponível (status: %d)", status_code);
            }
        }
        else
        {
            ESP_LOGE(TAG, "Erro ao verificar atualizações: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
        s_status = OTAStatus::IDLE;
        
        if (s_status_callback)
            s_status_callback(s_status, "Verificação concluída");
    }

    void OTAManager::start_update(const std::string &firmware_url)
    {
        if (s_status != OTAStatus::IDLE)
        {
            ESP_LOGW(TAG, "OTA já está em andamento");
            return;
        }

        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "INICIANDO ATUALIZAÇÃO OTA");
        ESP_LOGI(TAG, "URL: %s", firmware_url.c_str());
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

        s_status = OTAStatus::DOWNLOADING;
        s_download_progress = 0;
        
        if (s_status_callback)
            s_status_callback(s_status, "Iniciando download...");

        if (download_firmware(firmware_url))
        {
            ESP_LOGI(TAG, "Download concluído, instalando firmware...");
            s_status = OTAStatus::INSTALLING;
            if (s_status_callback)
                s_status_callback(s_status, "Instalando firmware...");

            if (install_firmware())
            {
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                ESP_LOGI(TAG, "ATUALIZAÇÃO CONCLUÍDA COM SUCESSO!");
                ESP_LOGI(TAG, "Reiniciando em 5 segundos...");
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                
                s_status = OTAStatus::SUCCESS;
                if (s_status_callback)
                    s_status_callback(s_status, "Atualização concluída com sucesso");
                
                vTaskDelay(pdMS_TO_TICKS(5000));
                esp_restart();
            }
            else
            {
                ESP_LOGE(TAG, "Falha ao instalar firmware");
                s_status = OTAStatus::FAILED;
                if (s_status_callback)
                    s_status_callback(s_status, "Falha ao instalar firmware");
            }
        }
        else
        {
            ESP_LOGE(TAG, "Falha ao baixar firmware");
            s_status = OTAStatus::FAILED;
            if (s_status_callback)
                s_status_callback(s_status, "Falha ao baixar firmware");
        }
    }

    bool OTAManager::download_firmware(const std::string &url)
    {
        // Configuração HTTP para OTA
        esp_http_client_config_t http_config = {};
        http_config.url = url.c_str();
        http_config.timeout_ms = 30000;
        http_config.buffer_size = 1024;
        http_config.skip_cert_common_name_check = true;  // Pula verificação de certificado
        
        // Configuração OTA
        esp_https_ota_config_t ota_config = {};
        ota_config.http_config = &http_config;
        
        esp_https_ota_handle_t https_ota_handle = nullptr;
        esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
        
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_https_ota_begin falhou: %s", esp_err_to_name(err));
            return false;
        }

        ESP_LOGI(TAG, "Download iniciado de: %s", url.c_str());
        
        while (1)
        {
            err = esp_https_ota_perform(https_ota_handle);
            if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
                break;
            
            // Pequeno delay para não sobrecarregar
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Download concluído com sucesso");
            esp_https_ota_finish(https_ota_handle);
            return true;
        }
        else
        {
            ESP_LOGE(TAG, "Erro no download: %s", esp_err_to_name(err));
            esp_https_ota_abort(https_ota_handle);
            return false;
        }
    }

    bool OTAManager::install_firmware()
    {
        // O esp_https_ota_finish já instalou o firmware automaticamente
        // Apenas marca como válido para evitar rollback
        esp_ota_mark_app_valid_cancel_rollback();
        
        ESP_LOGI(TAG, "Firmware instalado e marcado como válido");
        return true;
    }

    void OTAManager::handle_ota_packet(const std::string &json)
    {
        // Processa pacote OTA recebido da rede mesh
        Protocol::Packet pkt;
        if (!Protocol::parse(json, pkt))
        {
            ESP_LOGW(TAG, "Falha ao parsear pacote OTA");
            return;
        }

        if (pkt.method == "OTA_UPDATE" && pkt.type == Protocol::PacketType::REQUEST)
        {
            // Extrai URL do firmware do body
            // Formato esperado: {"firmware_url":"http://server/firmware.bin"}
            ESP_LOGI(TAG, "Pacote OTA recebido: %s", pkt.body.c_str());
            // Por enquanto, apenas loga
        }
    }

    void OTAManager::ota_task(void *param)
    {
        ESP_LOGI(TAG, "Task OTA iniciada (Gateway)");
        
        // Aguarda alguns segundos antes de verificar
        vTaskDelay(pdMS_TO_TICKS(10000));
        
        for (;;)
        {
            // Verifica atualizações a cada 1 hora
            check_for_update();
            vTaskDelay(pdMS_TO_TICKS(3600000));  // 1 hora
        }
    }
}

