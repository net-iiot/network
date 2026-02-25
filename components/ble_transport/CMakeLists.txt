idf_component_register(
    SRCS "ble_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_event nvs_flash protocol
    PRIV_REQUIRES network_manager led_manager
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
