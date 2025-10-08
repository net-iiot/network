idf_component_register(
    SRCS "network_manager.cpp" "test_packet_generator.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol router ble_transport gateway led_manager
)
