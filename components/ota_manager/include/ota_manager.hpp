#pragma once
#include <string>
#include <stdint.h>
#include <functional>
#include "protocol.hpp"

namespace WetzelMesh
{
    // Status do OTA
    enum class OTAStatus
    {
        IDLE,           // Nenhuma atualização em andamento
        CHECKING,       // Verificando nova versão
        DOWNLOADING,    // Baixando novo firmware
        INSTALLING,     // Instalando novo firmware
        SUCCESS,        // Atualização concluída com sucesso
        FAILED          // Falha na atualização
    };

    // Informações de versão do firmware
    struct FirmwareVersion
    {
        std::string version;      // Versão (ex: "1.0.0")
        std::string build_date;   // Data de build
        uint32_t build_number;    // Número de build
        std::string hash;         // Hash do firmware
    };

    class OTAManager
    {
    public:
        // Inicializa o OTA Manager
        static void init(bool isGateway, const std::string &server_url = "");
        
        // Verifica se há nova versão disponível
        static void check_for_update();
        
        // Inicia download e instalação de nova versão
        static void start_update(const std::string &firmware_url);
        
        // Obtém status atual do OTA
        static OTAStatus get_status();
        
        // Obtém versão atual do firmware
        static FirmwareVersion get_current_version();
        
        // Obtém progresso do download (0-100)
        static int get_download_progress();
        
        // Callback para notificar mudanças de status
        using StatusCallback = std::function<void(OTAStatus, const std::string &)>;
        static void set_status_callback(StatusCallback cb);

        // Registra callback para enviar pacote ao border node (usado pelo gateway)
        using SendToBorderCallback = std::function<bool(const Protocol::Packet &)>;
        static void set_send_to_border_callback(SendToBorderCallback cb);

        // Registra callback para reportar resultado de OTA ao servidor (sucesso ou falha)
        // Gateway: envia HTTP diretamente. Nodes: enviam pacote OTA_RESULT pela mesh.
        using ReportCallback = std::function<void(bool success, const std::string &command_id, const std::string &node_id, const std::string &error_msg)>;
        static void set_report_callback(ReportCallback cb);

        // Processa pacote OTA recebido da rede mesh
        static void handle_ota_packet(const std::string &json);

        // Processa comandos OTA recebidos do servidor (JSON com lista de comandos)
        static void process_ota_commands(const std::string &json);

        // Processa um comando OTA individual
        static void process_ota_command(const std::string &command_id, const std::string &target, const std::string &firmware_url, const std::string &version);
        
        // Task OTA (precisa ser pública para o wrapper C)
        static void ota_task(void *param);

    private:
        static bool download_firmware(const std::string &url);
        static bool install_firmware();
        static FirmwareVersion parse_version_info(const std::string &json);
        static std::string get_version_string();
        
        static bool s_isGateway;
        static std::string s_server_url;
        static OTAStatus s_status;
        static int s_download_progress;
        static FirmwareVersion s_current_version;
        static StatusCallback s_status_callback;
        static SendToBorderCallback s_send_to_border_callback;
        static ReportCallback s_report_callback;
        static std::string s_current_command_id; // ID do comando OTA em execução
    };
}

