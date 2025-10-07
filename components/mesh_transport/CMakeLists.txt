idf_component_register(
    SRCS "mesh_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt mbedtls nvs_flash uart_bridge
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
