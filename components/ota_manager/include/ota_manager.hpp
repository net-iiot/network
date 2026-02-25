#pragma once
#include <string>
#include <stdint.h>
#include <functional>
#include "protocol.hpp"

namespace NetworkMesh
{
    enum class OTAStatus
    {
        IDLE,
        CHECKING,
        DOWNLOADING,
        INSTALLING,
        SUCCESS,
        FAILED
    };

    struct FirmwareVersion
    {
        std::string version;
        std::string build_date;
        uint32_t build_number;
        std::string hash;
    };

    class OTAManager
    {
    public:
        static void init(bool isGateway, const std::string &server_url = "");
        static void check_for_update();
        static void start_update(const std::string &firmware_url);
        static OTAStatus get_status();
        static FirmwareVersion get_current_version();
        static int get_download_progress();

        using StatusCallback = std::function<void(OTAStatus, const std::string &)>;
        static void set_status_callback(StatusCallback cb);

        using SendToBorderCallback = std::function<bool(const Protocol::Packet &)>;
        static void set_send_to_border_callback(SendToBorderCallback cb);

        using ReportCallback = std::function<void(bool success, const std::string &command_id, const std::string &node_id, const std::string &error_msg)>;
        static void set_report_callback(ReportCallback cb);

        static void handle_ota_packet(const std::string &json);
        static void process_ota_commands(const std::string &json);
        static void process_ota_command(const std::string &command_id, const std::string &target, const std::string &firmware_url, const std::string &version);

    private:
        static bool download_firmware(const std::string &url);
        static bool install_firmware();
        static FirmwareVersion parse_version_info(const std::string &json);
        static std::string get_version_string();
        static void rollback_watchdog_task(void *param);
        static void save_ota_state(const std::string &command_id, const std::string &version);
        static void check_pending_ota_result();
        static void clear_ota_state();

        static bool s_isGateway;
        static std::string s_server_url;
        static OTAStatus s_status;
        static int s_download_progress;
        static FirmwareVersion s_current_version;
        static StatusCallback s_status_callback;
        static SendToBorderCallback s_send_to_border_callback;
        static ReportCallback s_report_callback;
        static std::string s_current_command_id;
    };
}
