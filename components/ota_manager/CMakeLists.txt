idf_component_register(
    SRCS "ota_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_https_ota esp_http_client app_update esp_app_format freertos nvs_flash
    REQUIRES protocol
)

