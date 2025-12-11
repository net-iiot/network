idf_component_register(
    SRCS "router.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol network_manager espnow_transport led_manager gateway
    PRIV_REQUIRES border_uart
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
