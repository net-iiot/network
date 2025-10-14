idf_component_register(
    SRCS "gateway.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol esp_driver_uart network_manager
    PRIV_REQUIRES bt
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
