idf_component_register(
    SRCS "border_uart.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver protocol led_manager
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
