idf_component_register(
    SRCS "router.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol gateway ble_transport network_manager led_manager
)
