idf_component_register(
    SRCS "network_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol espnow_transport ble_transport gateway led_manager mbedtls ota_manager
    PRIV_REQUIRES border_uart network_mapper
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
