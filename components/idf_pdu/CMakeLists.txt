idf_component_register(
  SRCS "pdulib.cpp"
  INCLUDE_DIRS "include"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE DESKTOP_PDU=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-implicit-fallthrough)
