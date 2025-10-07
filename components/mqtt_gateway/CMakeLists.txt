idf_component_register(
    SRCS "mqtt_gateway.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif mqtt
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
