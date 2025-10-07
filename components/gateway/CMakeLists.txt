idf_component_register(
    SRCS "gateway.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol router driver freertos
)
