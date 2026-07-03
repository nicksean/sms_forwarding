idf_component_register(
  SRCS "idf_config.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    freertos
    idf_logbuf
    nvs_flash
)

if(EXISTS "${PROJECT_DIR}/code/wifi_config.h")
  target_include_directories(${COMPONENT_LIB} PRIVATE "${PROJECT_DIR}/code")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE IDF_HAS_WIFI_CONFIG_H=1)
endif()
