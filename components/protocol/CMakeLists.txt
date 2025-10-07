idf_component_register(
    SRCS "protocol.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json_codec
)

set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
