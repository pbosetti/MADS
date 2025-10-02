#   ____  _             _       
#  |  _ \| |_   _  __ _(_)_ __  
#  | |_) | | | | |/ _` | | '_ \ 
#  |  __/| | |_| | (_| | | | | |
#  |_|   |_|\__,_|\__, |_|_| |_|
#                 |___/         
{% include "header.tpl" %}
cmake_minimum_required(VERSION 3.20)
project({{name}} VERSION {{mads_version}} LANGUAGES CXX)
if(CMAKE_BUILD_TYPE STREQUAL "")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "{{install_dir}}" CACHE PATH "Install path prefix, prepended onto install directories." FORCE)
endif()
message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
message(STATUS "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")
set(PLUGIN_SUFFIX "" CACHE STRING "Suffix for the plugin file, identifying the architecture, e.g. arm64, x86_64, etc.")
if(PLUGIN_SUFFIX STREQUAL "")
  message(WARNING "No PLUGIN_SUFFIX set, using default plugin name. In multi-platform environment with OTA plugins it is advised to set PLUGIN_SUFFIX to the architecture, e.g. arm64, x86_64, etc.")
else()
  message(STATUS "PLUGIN_SUFFIX set to: ${PLUGIN_SUFFIX}")
endif()

# PROJECT SETTINGS #############################################################
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

if(UNIX AND NOT APPLE)
  set(LINUX TRUE)
endif()

# DEPENDENCIES #################################################################
include(FetchContent)
# pugg is for the plugin system
FetchContent_Declare(pugg 
  GIT_REPOSITORY https://github.com/pbosetti/pugg.git
  GIT_TAG        1.0.2
  GIT_SHALLOW    TRUE
)

set(BUILD_TESTING OFF CACHE INTERNAL "")
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(pugg json)

FetchContent_Populate(plugin 
  GIT_REPOSITORY https://github.com/pbosetti/mads_plugin.git
  GIT_TAG        v1.3.5
  GIT_SHALLOW    TRUE
  SUBBUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/plugin-subbuild
  SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/plugin-src
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/plugin-build
)

include_directories(${plugin_SOURCE_DIR}/src)


# MACROS #######################################################################
# Call: add_plugin(name [SRCS src1 src2 ...] [LIBS lib1 lib2 ...])
#       the source file ${SRC_DIR}/plugin/<name>.cpp is implicitly added
macro(add_plugin name)
  # on MacOS only, plugins can be compiled as executables
  set(multiValueArgs LIBS SRCS)
  cmake_parse_arguments(plugin "" "" "${multiValueArgs}" ${ARGN})
  if (APPLE)
    add_executable(${name} ${SRC_DIR}/${name}.cpp ${plugin_SRCS})
    set_target_properties(${name} PROPERTIES ENABLE_EXPORTS TRUE)
    set(${name}_EXEC ${name}.plugin)
  else()
    add_library(${name} SHARED ${SRC_DIR}/${name}.cpp ${plugin_SRCS})
    add_executable(${name}_main ${SRC_DIR}/${name}.cpp ${plugin_SRCS})
    target_link_libraries(${name}_main PRIVATE pugg ${plugin_LIBS})
    set_target_properties(${name}_main PROPERTIES OUTPUT_NAME ${name})
    set(${name}_EXEC ${name})
    list(APPEND TARGET_LIST ${name}_main)
  endif()
  target_link_libraries(${name} PRIVATE pugg ${plugin_LIBS})
  set_target_properties(${name} PROPERTIES PREFIX "")
  if (PLUGIN_SUFFIX)
    set_target_properties(${name} PROPERTIES SUFFIX "_${PLUGIN_SUFFIX}.plugin")
    target_compile_definitions(${name} PRIVATE PLUGIN_NAME="${name}_${PLUGIN_SUFFIX}")
  else()
    set_target_properties(${name} PROPERTIES SUFFIX ".plugin")
    target_compile_definitions(${name} PRIVATE PLUGIN_NAME="${name}")
  endif()
  list(APPEND TARGET_LIST ${name})
endmacro()


# BUILD SETTINGS ###############################################################
if (APPLE)
  set(CMAKE_INSTALL_RPATH "@executable_path/../lib")
  include_directories(/opt/homebrew/include)
  link_directories(/opt/homebrew/lib)
else()
  set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib;/usr/local/lib")
endif()
include_directories(${json_SOURCE_DIR}/include)

# These plugins are always build and use for testing
add_plugin({{name}})


# INSTALL ######################################################################
if(APPLE)
  install(TARGETS ${TARGET_LIST}
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION lib
    ARCHIVE DESTINATION lib
  )
else()
  install(TARGETS ${TARGET_LIST}
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
  )
endif()
