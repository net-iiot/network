idf_component_register(
    SRCS "ble_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_event nvs_flash protocol router      # FIX: adicionamos protocol aqui
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
