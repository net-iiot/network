idf_component_register(
    SRCS "uart_bridge.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
