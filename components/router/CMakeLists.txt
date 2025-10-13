idf_component_register(
    SRCS "router.cpp"
    INCLUDE_DIRS "include"
    REQUIRES log protocol network_manager led_manager espnow_transport gateway
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
