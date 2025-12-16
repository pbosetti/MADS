message(STATUS "Loading libsodium")
include_guard(GLOBAL)
add_library(sodium::sodium STATIC IMPORTED)
set_target_properties(sodium::sodium PROPERTIES
  IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_LIST_DIR}/libsodium/x64/Release/v143/static/libsodium.lib"
  IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/libsodium/x64/Release/v143/static/libsodium.lib"
  IMPORTED_LOCATION_DEBUG "${CMAKE_CURRENT_LIST_DIR}/libsodium/x64/Debug/v143/static/libsodium.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/libsodium/include"
)
set(SODIUM_FOUND TRUE CACHE INTERNAL "")
set(SODIUM_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/libsodium/include" CACHE INTERNAL "")
set(SODIUM_LIBRARIES sodium::sodium CACHE INTERNAL "")