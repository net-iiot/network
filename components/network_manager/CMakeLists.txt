idf_component_register(
    SRCS "network_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol espnow_transport ble_transport gateway led_manager chunk_manager test_packet_generator mbedtls
    PRIV_REQUIRES border_uart
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
