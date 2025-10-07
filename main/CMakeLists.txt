idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        mesh_transport
        uart_bridge
        json_codec
        interpreters
        mqtt_gateway
        security
)

