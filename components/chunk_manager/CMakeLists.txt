idf_component_register(
    SRCS "chunk_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol
    PRIV_REQUIRES esp_timer
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)

