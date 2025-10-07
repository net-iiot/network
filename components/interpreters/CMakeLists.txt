idf_component_register(
    SRCS "interpreter.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json
)
target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions -frtti)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17)
