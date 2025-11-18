idf_component_register(
    SRCS "ota_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_https_ota esp_http_client freertos nvs_flash
    REQUIRES protocol network_manager gateway
)

