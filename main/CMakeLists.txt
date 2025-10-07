idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        json_codec
        security
        protocol
        router
        gateway
        router
        ble_transport
        network_manager
        led_manager
)

