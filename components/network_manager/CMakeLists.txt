idf_component_register(
    SRCS "network_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol router ble_transport gateway
)
