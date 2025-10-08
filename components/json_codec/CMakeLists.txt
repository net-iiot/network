idf_component_register(
    SRCS "json_codec.cpp"
    INCLUDE_DIRS "include"
    REQUIRES cjson
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
