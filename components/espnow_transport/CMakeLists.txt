idf_component_register(
    SRCS "espnow_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event nvs_flash protocol
    PRIV_REQUIRES network_manager border_uart led_manager ble_transport
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
