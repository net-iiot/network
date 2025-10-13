idf_component_register(SRCS "gateway.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES protocol network_manager esp_driver_uart
                                esp_http_client 
                                esp_wifi 
                                esp_netif 
                                esp_event)

set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)