idf_component_register(
    SRCS "ble_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash protocol router esp_hw_support
)

set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)