idf_component_register(
    SRCS "protocol.cpp" "validation.cpp"
    INCLUDE_DIRS "include"
    REQUIRES cjson
    PRIV_REQUIRES esp_timer
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)