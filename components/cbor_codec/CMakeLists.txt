idf_component_register(
    SRCS "cbor_codec.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
