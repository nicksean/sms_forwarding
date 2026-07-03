set(WEB_ASSETS_SRC "${CMAKE_CURRENT_LIST_DIR}/../../code/web_assets.cpp")
set(WEB_ASSETS_INC "${CMAKE_CURRENT_LIST_DIR}/../../code")

idf_component_register(
  SRCS "${WEB_ASSETS_SRC}"
  INCLUDE_DIRS "${WEB_ASSETS_INC}"
)
