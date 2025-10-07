idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        json_codec
        interpreters
        security
        protocol
        router
        gateway
)

