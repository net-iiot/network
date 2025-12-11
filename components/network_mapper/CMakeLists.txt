idf_component_register(
    SRCS "network_mapper.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer freertos border_uart
    REQUIRES network_manager protocol
)

