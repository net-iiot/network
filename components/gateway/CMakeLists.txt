idf_component_register(
    SRCS "http_context.cpp" "gateway.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol esp_driver_uart network_manager esp_http_client esp_wifi esp_netif nvs_flash
    PRIV_REQUIRES bt esp_timer
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
