idf_component_register(
    SRCS "interpreter_registry.cpp" "telemetry_v1.cpp"
    INCLUDE_DIRS "include"
)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
