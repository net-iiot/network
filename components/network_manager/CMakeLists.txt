idf_component_register(
    SRCS "network_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_wifi esp_timer esp_netif esp_http_client protocol espnow_transport ble_transport
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
