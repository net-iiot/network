idf_component_register(
    SRCS "test_packet_generator.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        freertos
        esp_timer
        esp_event
        esp_wifi
        nvs_flash
        protocol
        network_manager
        led_manager
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
