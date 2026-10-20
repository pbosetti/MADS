# mads.ini document model, key schema and `mads ini` CLI. GUI-free, so the
# `mads` dispatcher (headless builds included) and mads-ini share it.

# Embed ini_schema.toml as a byte array: a raw string literal would hit MSVC's
# 16 KiB per-literal limit. Re-configures whenever the schema changes.
set(_ini_schema_src ${CMAKE_CURRENT_SOURCE_DIR}/ini_schema.toml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ini_schema_src})
file(READ ${_ini_schema_src} _ini_schema_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _ini_schema_bytes "${_ini_schema_hex}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ini_schema_data.hpp
  "// Generated from ini_schema.toml by src/ini/CMakeLists.txt -- do not edit.\n"
  "#pragma once\n"
  "static const unsigned char kIniSchemaData[] = {${_ini_schema_bytes}0x00};\n"
)

add_library(mads_ini_core STATIC
  ini_cli.cpp
  ini_document.cpp
  ini_editor.cpp
  ini_schema.cpp
  remote_edit.cpp
)
target_include_directories(mads_ini_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(mads_ini_core PUBLIC MadsCore MadsMainDeps)
