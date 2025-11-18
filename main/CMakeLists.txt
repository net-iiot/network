idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        test_packet_generator
        router
        protocol
        network_manager
        network_mapper
        ota_manager
        led_manager
        ble_transport
        espnow_transport
        gateway
        security
        nvs_flash
        esp_wifi
        freertos
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
