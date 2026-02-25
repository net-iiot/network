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
#include "protocol.hpp"
#include "cJSON.h"
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace NetworkMesh
{
    static const char *TAG = "OTA";
    static const char *NVS_NAMESPACE = "ota_state";
    static const char *NVS_KEY_BOOT_COUNT = "boot_cnt";
    static const char *NVS_KEY_COMMAND_ID = "cmd_id";
    static const char *NVS_KEY_PREV_VERSION = "prev_ver";
    static const int MAX_BOOT_RETRIES = 3;

    bool OTAManager::s_isGateway = false;
    std::string OTAManager::s_server_url = "";
    OTAStatus OTAManager::s_status = OTAStatus::IDLE;
    int OTAManager::s_download_progress = 0;
    FirmwareVersion OTAManager::s_current_version;
    OTAManager::StatusCallback OTAManager::s_status_callback = nullptr;
    OTAManager::SendToBorderCallback OTAManager::s_send_to_border_callback = nullptr;
    OTAManager::ReportCallback OTAManager::s_report_callback = nullptr;
    std::string OTAManager::s_current_command_id = "";

    // ─── NVS helpers ────────────────────────────────────────────────

    static bool nvs_read_string(nvs_handle_t h, const char *key, std::string &out)
    {
        size_t len = 0;
        if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0)
            return false;
        char *buf = (char *)malloc(len);
        if (!buf)
            return false;
        esp_err_t err = nvs_get_str(h, key, buf, &len);
        if (err == ESP_OK)
            out.assign(buf, len - 1);
        free(buf);
        return err == ESP_OK;
    }

    // ─── Rollback & NVS state ───────────────────────────────────────

    void OTAManager::save_ota_state(const std::string &command_id, const std::string &version)
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
            return;
        nvs_set_str(h, NVS_KEY_COMMAND_ID, command_id.c_str());
        nvs_set_str(h, NVS_KEY_PREV_VERSION, version.c_str());
        nvs_set_u8(h, NVS_KEY_BOOT_COUNT, 0);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Estado OTA salvo no NVS (cmd=%s, ver=%s)", command_id.c_str(), version.c_str());
    }

    void OTAManager::clear_ota_state()
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
            return;
        nvs_erase_key(h, NVS_KEY_COMMAND_ID);
        nvs_erase_key(h, NVS_KEY_PREV_VERSION);
        nvs_set_u8(h, NVS_KEY_BOOT_COUNT, 0);
        nvs_commit(h);
        nvs_close(h);
    }

    void OTAManager::check_pending_ota_result()
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
            return;

        std::string saved_cmd_id, prev_version;
        bool has_state = nvs_read_string(h, NVS_KEY_COMMAND_ID, saved_cmd_id);
        nvs_read_string(h, NVS_KEY_PREV_VERSION, prev_version);
        nvs_close(h);

        if (!has_state || saved_cmd_id.empty())
            return;

        bool version_changed = (!prev_version.empty() && prev_version != s_current_version.version);

        ESP_LOGI(TAG, "Estado OTA pendente encontrado: cmd=%s, prev_ver=%s, cur_ver=%s",
                 saved_cmd_id.c_str(), prev_version.c_str(), s_current_version.version.c_str());

        if (version_changed)
        {
            ESP_LOGI(TAG, "OTA bem sucedido! Versão mudou de %s para %s", prev_version.c_str(), s_current_version.version.c_str());
            if (s_report_callback)
            {
                std::string node_id = s_isGateway ? "gateway" : "";
                s_report_callback(true, saved_cmd_id, node_id, "");
            }
        }
        else
        {
            ESP_LOGW(TAG, "OTA pode ter falhado — versão não mudou (rollback?)");
            if (s_report_callback)
            {
                std::string node_id = s_isGateway ? "gateway" : "";
                s_report_callback(false, saved_cmd_id, node_id, "Versão não mudou após reboot (possível rollback)");
            }
        }

        clear_ota_state();
    }

    void OTAManager::rollback_watchdog_task(void *param)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));

        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Firmware confirmado como válido após 10s de uptime estável");

            nvs_handle_t h;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK)
            {
                nvs_set_u8(h, NVS_KEY_BOOT_COUNT, 0);
                nvs_commit(h);
                nvs_close(h);
            }
        }
        else
        {
            ESP_LOGW(TAG, "esp_ota_mark_app_valid falhou: %s (pode não ser partição OTA)", esp_err_to_name(err));
        }

        vTaskDelete(nullptr);
    }

    // ─── Callbacks ──────────────────────────────────────────────────

    void OTAManager::set_send_to_border_callback(SendToBorderCallback cb)
    {
        s_send_to_border_callback = cb;
    }

    void OTAManager::set_report_callback(ReportCallback cb)
    {
        s_report_callback = cb;
    }

    // ─── Version info ───────────────────────────────────────────────

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

        std::string idf_ver_str = app_desc->idf_ver;
        uint32_t hash = 0;
        for (char c : idf_ver_str)
            hash = hash * 31 + c;
        s_current_version.build_number = hash;

        std::ostringstream oss;
        oss << app_desc->version << app_desc->date << app_desc->time;
        s_current_version.hash = oss.str();

        return s_current_version;
    }

    FirmwareVersion OTAManager::parse_version_info(const std::string &json)
    {
        FirmwareVersion version;
        cJSON *root = cJSON_Parse(json.c_str());
        if (!root)
            return version;

        cJSON *jver = cJSON_GetObjectItem(root, "version");
        if (jver && cJSON_IsString(jver))
            version.version = jver->valuestring;

        cJSON *jdate = cJSON_GetObjectItem(root, "build_date");
        if (jdate && cJSON_IsString(jdate))
            version.build_date = jdate->valuestring;

        cJSON *jbuild = cJSON_GetObjectItem(root, "build_number");
        if (jbuild && cJSON_IsNumber(jbuild))
            version.build_number = (uint32_t)jbuild->valuedouble;

        cJSON *jhash = cJSON_GetObjectItem(root, "hash");
        if (jhash && cJSON_IsString(jhash))
            version.hash = jhash->valuestring;

        cJSON_Delete(root);
        return version;
    }

    // ─── Init ───────────────────────────────────────────────────────

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

        // ── Rollback: boot counter ──
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK)
        {
            uint8_t boot_count = 0;
            nvs_get_u8(h, NVS_KEY_BOOT_COUNT, &boot_count);
            boot_count++;
            nvs_set_u8(h, NVS_KEY_BOOT_COUNT, boot_count);
            nvs_commit(h);
            nvs_close(h);

            ESP_LOGI(TAG, "Boot count: %d/%d", boot_count, MAX_BOOT_RETRIES);

            if (boot_count >= MAX_BOOT_RETRIES)
            {
                ESP_LOGE(TAG, "Boot count atingiu limite (%d)! Tentando rollback...", MAX_BOOT_RETRIES);
                if (esp_ota_check_rollback_is_possible())
                {
                    esp_ota_mark_app_invalid_rollback_and_reboot();
                    // Não retorna — reboot imediato
                }
                else
                {
                    ESP_LOGW(TAG, "Rollback não disponível, continuando com firmware atual");
                    // Zera contador para não ficar preso
                    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK)
                    {
                        nvs_set_u8(h, NVS_KEY_BOOT_COUNT, 0);
                        nvs_commit(h);
                        nvs_close(h);
                    }
                }
            }
        }

        // ── Rollback watchdog: confirma firmware após 10s estável ──
        xTaskCreatePinnedToCore(rollback_watchdog_task, "ota_rollback", 2048, nullptr, 3, nullptr, tskNO_AFFINITY);

        // ── Verificar resultado de OTA pendente (após reboot) ──
        // Nota: check_pending_ota_result depende de s_report_callback.
        // No node, o callback é setado depois em network_manager::init().
        // Então adiamos essa verificação com uma task curta.
        xTaskCreatePinnedToCore(
            [](void *)
            {
                vTaskDelay(pdMS_TO_TICKS(5000));
                check_pending_ota_result();
                vTaskDelete(nullptr);
            },
            "ota_pending", 3072, nullptr, 3, nullptr, tskNO_AFFINITY);
    }

    // ─── Status ─────────────────────────────────────────────────────

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

    // ─── Check for update ───────────────────────────────────────────

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

        std::string version_url = s_server_url + "/api/firmware/version";

        std::string response_body;

        auto http_event = [](esp_http_client_event_t *evt) -> esp_err_t
        {
            if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data)
            {
                std::string *body = static_cast<std::string *>(evt->user_data);
                body->append((char *)evt->data, evt->data_len);
            }
            return ESP_OK;
        };

        esp_http_client_config_t config = {};
        config.url = version_url.c_str();
        config.timeout_ms = 5000;
        config.event_handler = http_event;
        config.user_data = &response_body;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client)
        {
            ESP_LOGE(TAG, "Falha ao criar cliente HTTP");
            s_status = OTAStatus::IDLE;
            return;
        }

        esp_http_client_set_method(client, HTTP_METHOD_GET);

        ESP_LOGI(TAG, "Fazendo GET para: %s", version_url.c_str());
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK)
        {
            int status_code = esp_http_client_get_status_code(client);

            if (status_code == 200 && !response_body.empty())
            {
                ESP_LOGI(TAG, "Resposta do servidor: %s", response_body.c_str());

                FirmwareVersion available = parse_version_info(response_body);

                if (available.version.empty())
                {
                    ESP_LOGW(TAG, "Resposta do servidor sem campo 'version'");
                }
                else if (available.version == s_current_version.version)
                {
                    ESP_LOGI(TAG, "Firmware já está na versão mais recente: %s", available.version.c_str());
                }
                else
                {
                    ESP_LOGI(TAG, "Nova versão disponível: %s (atual: %s)", available.version.c_str(), s_current_version.version.c_str());

                    cJSON *root = cJSON_Parse(response_body.c_str());
                    if (root)
                    {
                        cJSON *jurl = cJSON_GetObjectItem(root, "url");
                        if (jurl && cJSON_IsString(jurl))
                        {
                            std::string firmware_url = jurl->valuestring;
                            cJSON_Delete(root);
                            esp_http_client_cleanup(client);
                            s_status = OTAStatus::IDLE;
                            start_update(firmware_url);
                            return;
                        }
                        cJSON_Delete(root);
                    }
                    ESP_LOGW(TAG, "Nova versão disponível mas sem 'url' na resposta");
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

            if (err == ESP_ERR_HTTP_CONNECT)
            {
                ESP_LOGW(TAG, "Servidor não acessível. Verifique:");
                ESP_LOGW(TAG, "  1. Servidor está rodando?");
                ESP_LOGW(TAG, "  2. URL está correta? (%s)", version_url.c_str());
                ESP_LOGW(TAG, "  3. WiFi está conectado?");
            }
        }

        esp_http_client_cleanup(client);
        s_status = OTAStatus::IDLE;

        if (s_status_callback)
            s_status_callback(s_status, "Verificação concluída");
    }

    // ─── Start update ───────────────────────────────────────────────

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

        // Salvar estado no NVS antes de iniciar (para reportar resultado após reboot)
        save_ota_state(s_current_command_id, s_current_version.version);

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

                if (s_report_callback && !s_current_command_id.empty())
                {
                    std::string node_id = s_isGateway ? "gateway" : "";
                    s_report_callback(true, s_current_command_id, node_id, "");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }

                vTaskDelay(pdMS_TO_TICKS(3000));
                esp_restart();
            }
            else
            {
                ESP_LOGE(TAG, "Falha ao instalar firmware");
                s_status = OTAStatus::FAILED;
                if (s_status_callback)
                    s_status_callback(s_status, "Falha ao instalar firmware");
                if (s_report_callback && !s_current_command_id.empty())
                {
                    std::string node_id = s_isGateway ? "gateway" : "";
                    s_report_callback(false, s_current_command_id, node_id, "Falha ao instalar firmware");
                }
                clear_ota_state();
            }
        }
        else
        {
            ESP_LOGE(TAG, "Falha ao baixar firmware");
            s_status = OTAStatus::FAILED;
            if (s_status_callback)
                s_status_callback(s_status, "Falha ao baixar firmware");
            if (s_report_callback && !s_current_command_id.empty())
            {
                std::string node_id = s_isGateway ? "gateway" : "";
                s_report_callback(false, s_current_command_id, node_id, "Falha ao baixar firmware");
            }
            clear_ota_state();
        }

        s_current_command_id.clear();
        s_status = OTAStatus::IDLE;
    }

    // ─── Download & Install ─────────────────────────────────────────

    bool OTAManager::download_firmware(const std::string &url)
    {
        esp_http_client_config_t http_config = {};
        http_config.url = url.c_str();
        http_config.timeout_ms = 30000;
        http_config.buffer_size = 1024;
        http_config.skip_cert_common_name_check = true;

        esp_https_ota_config_t ota_config = {};
        ota_config.http_config = &http_config;

        esp_https_ota_handle_t https_ota_handle = nullptr;
        esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_https_ota_begin falhou: %s", esp_err_to_name(err));
            return false;
        }

        int image_size = esp_https_ota_get_image_size(https_ota_handle);
        ESP_LOGI(TAG, "Download iniciado de: %s (tamanho: %d bytes)", url.c_str(), image_size);

        while (1)
        {
            err = esp_https_ota_perform(https_ota_handle);
            if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
                break;

            if (image_size > 0)
            {
                int read_len = esp_https_ota_get_image_len_read(https_ota_handle);
                s_download_progress = (read_len * 100) / image_size;
                ESP_LOGI(TAG, "Download: %d%%", s_download_progress);
            }

            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (err == ESP_OK)
        {
            s_download_progress = 100;
            ESP_LOGI(TAG, "Download concluído com sucesso");
            esp_err_t finish_err = esp_https_ota_finish(https_ota_handle);
            if (finish_err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_https_ota_finish falhou: %s", esp_err_to_name(finish_err));
                return false;
            }
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
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao marcar firmware como válido: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "Firmware instalado e marcado como válido");
        return true;
    }

    // ─── OTA packet handling ────────────────────────────────────────

    void OTAManager::handle_ota_packet(const std::string &json)
    {
        Protocol::Packet pkt;
        if (!Protocol::parse(json, pkt))
        {
            ESP_LOGW(TAG, "Falha ao parsear pacote OTA");
            return;
        }

        if ((pkt.method == "OTA_UPDATE" || pkt.method == "OTA_START") &&
            pkt.type == Protocol::PacketType::REQUEST)
        {
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
            ESP_LOGI(TAG, "PROCESSANDO COMANDO OTA RECEBIDO");
            ESP_LOGI(TAG, "   Método: %s", pkt.method.c_str());
            ESP_LOGI(TAG, "   Body: %s", pkt.body.c_str());
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

            cJSON *root = cJSON_Parse(pkt.body.c_str());
            if (!root)
            {
                ESP_LOGW(TAG, "Falha ao parsear body OTA como JSON");
                return;
            }

            std::string firmware_url, version, target, command_id;

            cJSON *jurl = cJSON_GetObjectItem(root, "firmware_url");
            if (jurl && cJSON_IsString(jurl))
                firmware_url = jurl->valuestring;

            cJSON *jver = cJSON_GetObjectItem(root, "version");
            if (jver && cJSON_IsString(jver))
                version = jver->valuestring;

            cJSON *jtarget = cJSON_GetObjectItem(root, "target");
            if (jtarget && cJSON_IsString(jtarget))
                target = jtarget->valuestring;

            cJSON *jcmd = cJSON_GetObjectItem(root, "command_id");
            if (jcmd && cJSON_IsString(jcmd))
                command_id = jcmd->valuestring;

            cJSON_Delete(root);

            if (!firmware_url.empty())
            {
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                ESP_LOGI(TAG, "INICIANDO OTA");
                ESP_LOGI(TAG, "   URL: %s", firmware_url.c_str());
                ESP_LOGI(TAG, "   Versão: %s", version.c_str());
                ESP_LOGI(TAG, "   Target: %s", target.c_str());
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

                s_current_command_id = command_id;
                start_update(firmware_url);
            }
            else
            {
                ESP_LOGW(TAG, "Comando OTA sem firmware_url, ignorando");
            }
        }
    }

    // ─── OTA command processing ─────────────────────────────────────

    void OTAManager::process_ota_commands(const std::string &json)
    {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "PROCESSANDO COMANDOS OTA DO SERVIDOR");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

        cJSON *root = cJSON_Parse(json.c_str());
        if (!root)
        {
            ESP_LOGW(TAG, "Formato de comandos OTA inválido (JSON parse falhou)");
            return;
        }

        cJSON *commands = cJSON_GetObjectItem(root, "commands");
        if (!commands || !cJSON_IsArray(commands))
        {
            ESP_LOGW(TAG, "Formato de comandos OTA inválido (sem array 'commands')");
            cJSON_Delete(root);
            return;
        }

        int count = cJSON_GetArraySize(commands);
        for (int i = 0; i < count; i++)
        {
            cJSON *cmd = cJSON_GetArrayItem(commands, i);
            if (!cmd)
                continue;

            std::string command_id, target, firmware_url, version;

            cJSON *jid = cJSON_GetObjectItem(cmd, "command_id");
            if (jid && cJSON_IsString(jid))
                command_id = jid->valuestring;

            cJSON *jtarget = cJSON_GetObjectItem(cmd, "target");
            if (jtarget && cJSON_IsString(jtarget))
                target = jtarget->valuestring;

            cJSON *jurl = cJSON_GetObjectItem(cmd, "firmware_url");
            if (jurl && cJSON_IsString(jurl))
                firmware_url = jurl->valuestring;

            cJSON *jver = cJSON_GetObjectItem(cmd, "version");
            if (jver && cJSON_IsString(jver))
                version = jver->valuestring;

            if (!target.empty() && !firmware_url.empty())
            {
                ESP_LOGI(TAG, "Comando OTA encontrado:");
                ESP_LOGI(TAG, "   ID: %s", command_id.c_str());
                ESP_LOGI(TAG, "   Target: %s", target.c_str());
                ESP_LOGI(TAG, "   URL: %s", firmware_url.c_str());
                ESP_LOGI(TAG, "   Versão: %s", version.c_str());

                process_ota_command(command_id, target, firmware_url, version);
            }
        }

        cJSON_Delete(root);
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    }

    void OTAManager::process_ota_command(const std::string &command_id, const std::string &target, const std::string &firmware_url, const std::string &version)
    {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "PROCESSANDO COMANDO OTA");
        ESP_LOGI(TAG, "   ID: %s", command_id.c_str());
        ESP_LOGI(TAG, "   Target: %s", target.c_str());
        ESP_LOGI(TAG, "   URL: %s", firmware_url.c_str());
        ESP_LOGI(TAG, "   Versão: %s", version.c_str());
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

        if (target == "gateway")
        {
            if (s_isGateway)
            {
                ESP_LOGI(TAG, "Gateway iniciando OTA próprio...");
                s_current_command_id = command_id;
                start_update(firmware_url);
            }
            else
            {
                ESP_LOGW(TAG, "Comando OTA para gateway recebido em node (ignorando)");
            }
        }
        else if (target == "border" || target == "all_nodes")
        {
            if (s_isGateway)
            {
                ESP_LOGI(TAG, "Gateway distribuindo comando OTA para nodes...");

                Protocol::Packet ota_cmd;
                ota_cmd.type = Protocol::PacketType::REQUEST;
                ota_cmd.method = "OTA_START";
                ota_cmd.route.src = "gateway";
                ota_cmd.route.dst = (target == "border") ? "border" : "broadcast";

                std::ostringstream body;
                body << R"({"firmware_url":")" << firmware_url
                     << R"(","version":")" << version
                     << R"(","target":")" << target
                     << R"(","command_id":")" << command_id << "\"}";
                ota_cmd.body = body.str();

                if (s_send_to_border_callback)
                {
                    bool sent = s_send_to_border_callback(ota_cmd);
                    if (sent)
                    {
                        ESP_LOGI(TAG, "Comando OTA enviado para border node via UART");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Falha ao enviar comando OTA via UART");
                        if (s_report_callback)
                            s_report_callback(false, command_id, target, "Falha ao enviar via UART");
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "Callback de envio para border não configurado");
                    if (s_report_callback)
                        s_report_callback(false, command_id, target, "Border callback não configurado");
                }
            }
            else
            {
                ESP_LOGW(TAG, "Comando OTA para nodes recebido em node (deveria ser gateway)");
            }
        }
        else
        {
            if (s_isGateway)
            {
                ESP_LOGI(TAG, "Gateway distribuindo comando OTA para node específico: %s", target.c_str());

                Protocol::Packet ota_cmd;
                ota_cmd.type = Protocol::PacketType::REQUEST;
                ota_cmd.method = "OTA_START";
                ota_cmd.route.src = "gateway";
                ota_cmd.route.dst = target;

                std::ostringstream body;
                body << R"({"firmware_url":")" << firmware_url
                     << R"(","version":")" << version
                     << R"(","target":")" << target
                     << R"(","command_id":")" << command_id << "\"}";
                ota_cmd.body = body.str();

                if (s_send_to_border_callback)
                {
                    bool sent = s_send_to_border_callback(ota_cmd);
                    if (sent)
                    {
                        ESP_LOGI(TAG, "Comando OTA enviado para node %s via border", target.c_str());
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Falha ao enviar comando OTA para node %s via UART", target.c_str());
                        if (s_report_callback)
                            s_report_callback(false, command_id, target, "Falha ao enviar via UART");
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "Callback de envio para border não configurado");
                    if (s_report_callback)
                        s_report_callback(false, command_id, target, "Border callback não configurado");
                }
            }
            else
            {
                ESP_LOGI(TAG, "Node recebeu comando OTA direto, iniciando atualização...");
                s_current_command_id = command_id;
                start_update(firmware_url);
            }
        }
    }
}
