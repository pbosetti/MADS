#
#  ____       _
# / ___|  ___| |_ _   _ _ __
# \___ \ / _ \ __| | | | '_ \
#  ___) |  __/ |_| |_| | |_) |
# |____/ \___|\__|\__,_| .__/
#                      |_|
#
cmake_minimum_required(VERSION 3.22)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
set(PROJECT_NAME Mads)
set(LIB_NAME ${PROJECT_NAME}Core)

# Version from git tags
# Check if project root is a git repository
execute_process(
  COMMAND git rev-parse --is-inside-work-tree
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  OUTPUT_VARIABLE IS_GIT_REPO
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(IS_GIT_REPO STREQUAL "true")
  # Get the latest tag. We only ever anchor on official release tags
  # (vMAJOR.MINOR.PATCH), so --exclude=*-g* skips any stray describe-style
  # pre-release tags that may exist in the history.
  execute_process(
    COMMAND git describe --tags --dirty --exclude=*-g*
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  # On an exact release tag this is just "v1.2.3". On commits past a tag (i.e.
  # manually dispatched, unofficial builds) git describe appends "-<n>-g<hash>";
  # mark these as pre-releases by turning that into "-p<n>-g<hash>", e.g.
  # v1.2.3-p3-gabc1234.
  string(REGEX REPLACE "^(v[0-9]+\\.[0-9]+\\.[0-9]+)-([0-9]+)-g" "\\1-p\\2-g"
         GIT_TAG "${GIT_TAG}")
  if(GIT_TAG MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(.*)?$")
    string(REGEX REPLACE "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(.*)?$" "\\1" CPACK_PACKAGE_VERSION_MAJOR "${GIT_TAG}")
    string(REGEX REPLACE "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(.*)?$" "\\2" CPACK_PACKAGE_VERSION_MINOR "${GIT_TAG}")
    string(REGEX REPLACE "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(.*)?$" "\\3" CPACK_PACKAGE_VERSION_PATCH "${GIT_TAG}")
    message(STATUS "Project root is a git repository, using version from latest tag: ${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
  else()
    message(WARNING "Latest git tag does not match vMAJOR.MINOR.PATCH format, using default version")
    set(CPACK_PACKAGE_VERSION_MAJOR 0)
    set(CPACK_PACKAGE_VERSION_MINOR 0)
    set(CPACK_PACKAGE_VERSION_PATCH 1)
  endif()
else()
  message(WARNING "Project root is NOT a git repository, using default version")
  set(CPACK_PACKAGE_VERSION_MAJOR 0)
  set(CPACK_PACKAGE_VERSION_MINOR 0)
  set(CPACK_PACKAGE_VERSION_PATCH 1)
  set(GIT_TAG "vUNKNOWN_GIT_VERSION")
endif()

# Set variables for project
set(LIB_VERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
set(LIB_VERSION_CHECK "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}")
project(${PROJECT_NAME} VERSION ${LIB_VERSION} LANGUAGES CXX C)
if (APPLE)
  enable_language(OBJC)
endif()
set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build.")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

if(APPLE)
  add_link_options(-Wl,-no_warn_duplicate_libraries)
endif()

include(GNUInstallDirs)
option(MADS_ENABLE_MONGOCXX "Enable MongoDB C++ driver dependency" ON)
option(MADS_BUILD_APPS "Build executables" ${PROJECT_IS_TOP_LEVEL})
option(MADS_DIRECTOR "Also build the director app (requires graphical interface)" ON)

set(APP_NAME ${LIB_NAME})
set(PREFIX "mads")

#
#  ____
# |  _ \  ___   ___ ___
# | | | |/ _ \ / __/ __|
# | |_| | (_) | (__\__ \
# |____/ \___/ \___|___/
#
find_package(Doxygen QUIET)
set(MADS_DOXYGEN_SOURCE "${CMAKE_CURRENT_LIST_DIR}/doc/Doxyfile")

if(DOXYGEN_FOUND)
  add_custom_target(doc
    COMMAND "${DOXYGEN_EXECUTABLE}" "${MADS_DOXYGEN_SOURCE}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    COMMENT "Generating Doxygen documentation in ${CMAKE_CURRENT_LIST_DIR}/doc"
    VERBATIM
  )
else()
  add_custom_target(doc
    COMMAND "${CMAKE_COMMAND}" -E echo
      "Doxygen was not found. Install doxygen to build the doc target."
    COMMAND "${CMAKE_COMMAND}" -E false
    VERBATIM
  )
endif()

# List of filenames to be configured
set(CONFIG_FILENAMES
  ${CMAKE_CURRENT_LIST_DIR}/Dockerfile
  ${SOURCE_DIR}/mads.hpp
)
# Loop over the filenames
foreach(filename ${CONFIG_FILENAMES})
  configure_file(
    ${filename}.in
    ${filename}
    @ONLY
  )
endforeach()

#
#  ____                 
# |  _ \  ___ _ __  ___
# | | | |/ _ \ '_ \/ __|
# | |_| |  __/ |_) \__ \
# |____/ \___| .__/|___/
#            |_|
#
add_subdirectory(vendors)
if(MADS_DIRECTOR)
  set(GLFW_BUILD_DOCS OFF)
  set(GLFW_LIBRARY_TYPE "STATIC")
  set(MADS_DIRECTOR_EXAMPLE_CONFIG ON)
  FetchContent_Declare(mads-director
    GIT_REPOSITORY https://github.com/mads-net/mads_director.git
    GIT_TAG v2.2.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(mads-director)
  if (MSVC)
    target_compile_options(glfw PUBLIC /w)
  else()
    target_compile_options(glfw PUBLIC -w)
  endif()
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /wd4505 /wd4005 /wd4244 /wd4996 /wd4459)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-deprecated-literal-operator)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if (MADS_ENABLE_TOOLS)
  add_subdirectory(tools)
endif()

#
#  _____                 _   _
# |  ___|   _ _ __   ___| |_(_) ___  _ __  ___
# | |_ | | | | '_ \ / __| __| |/ _ \| '_ \/ __|
# |  _|| |_| | | | | (__| |_| | (_) | | | \__ \
# |_|   \__,_|_| |_|\___|\__|_|\___/|_| |_|___/
#
function(link_first_available target)
  set(options)
  set(one_value_args LINK_SCOPE)
  set(multi_value_args CANDIDATES)
  cmake_parse_arguments(LFA "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if(NOT LFA_LINK_SCOPE)
    set(LFA_LINK_SCOPE PRIVATE)
  endif()

  foreach(candidate IN LISTS LFA_CANDIDATES)
    if(TARGET "${candidate}")
      target_link_libraries("${target}" ${LFA_LINK_SCOPE} "${candidate}")
      return()
    endif()
  endforeach()

  message(WARNING "No candidate target found for ${target}: ${LFA_CANDIDATES}")
endfunction()

#
#  _____                    _
# |_   _|_ _ _ __ __ _  ___| |_ ___
#   | |/ _` | '__/ _` |/ _ \ __/ __|
#   | | (_| | | | (_| |  __/ |_\__ \
#   |_|\__,_|_|  \__, |\___|\__|___/
#                |___/
#

file(GLOB LIB_SOURCES ${SOURCE_DIR}/*.cpp ${SOURCE_DIR}/*.mm)
if(NOT MSVC)
  add_compile_options(-Wno-unused-function)
endif()

if(ANDROID)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/mongo_fetch.cpp)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/https_client_linux.cpp)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/https_client_windows.cpp)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/https_client_macos.mm)
elseif(WIN32)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/https_client_linux.cpp)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/https_client_macos.mm)
elseif(APPLE)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/https_client_linux.cpp)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/https_client_windows.cpp)
else()
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/https_client_windows.cpp)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/https_client_macos.mm)
endif()

add_library(
  ${LIB_NAME}
  SHARED
  ${LIB_SOURCES}
)
add_library(Mads::Mads ALIAS ${LIB_NAME})
set_target_properties(${LIB_NAME} PROPERTIES EXPORT_NAME Mads)
if(WIN32)
  set_target_properties(${LIB_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_include_directories(
  ${LIB_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

link_first_available(${LIB_NAME} CANDIDATES nlohmann_json::nlohmann_json)
link_first_available(${LIB_NAME} CANDIDATES cxxopts)
link_first_available(${LIB_NAME} CANDIDATES rang::rang rang)
link_first_available(
  ${LIB_NAME}
  CANDIDATES
    tomlplusplus::tomlplusplus
    tomlplusplus::tomlplusplus_header_only
)
link_first_available(${LIB_NAME} CANDIDATES pugg::pugg pugg)
link_first_available(${LIB_NAME} CANDIDATES sodium sodium::sodium)
link_first_available(${LIB_NAME} CANDIDATES libzmq libzmq-static zmq)
link_first_available(${LIB_NAME} CANDIDATES zmqpp-static zmqpp)
if(TARGET zmqpp-static)
  get_target_property(_zmqpp_source_dir zmqpp-static SOURCE_DIR)
  get_target_property(_zmqpp_binary_dir zmqpp-static BINARY_DIR)
elseif(TARGET zmqpp)
  get_target_property(_zmqpp_source_dir zmqpp SOURCE_DIR)
  get_target_property(_zmqpp_binary_dir zmqpp BINARY_DIR)
endif()
if(DEFINED _zmqpp_source_dir AND _zmqpp_source_dir)
  target_include_directories(${LIB_NAME} PRIVATE "${_zmqpp_source_dir}/src")
endif()
if(DEFINED _zmqpp_binary_dir AND _zmqpp_binary_dir)
  target_include_directories(${LIB_NAME} PRIVATE "${_zmqpp_binary_dir}")
endif()
link_first_available(${LIB_NAME} CANDIDATES inja::inja inja)
link_first_available(${LIB_NAME} CANDIDATES Snappy::snappy snappy)
if(MADS_ENABLE_MONGOCXX)
  link_first_available(
    ${LIB_NAME}
    CANDIDATES
      mongo::mongocxx_static
      mongocxx_static
  )
  link_first_available(
    ${LIB_NAME}
    CANDIDATES
      mongo::bsoncxx_static
      bsoncxx_static
  )
endif()
if(APPLE)
  find_library(FOUNDATION_FRAMEWORK Foundation)
  if(FOUNDATION_FRAMEWORK)
    target_link_libraries(MadsCore PRIVATE "${FOUNDATION_FRAMEWORK}")
  endif()
endif()

add_library(MadsMainDeps INTERFACE)
if(WIN32)
  target_compile_definitions(${LIB_NAME} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _HAS_STD_BYTE=0)
  target_link_libraries(${LIB_NAME} PRIVATE ws2_32 iphlpapi winhttp)
  target_compile_definitions(MadsMainDeps INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _HAS_STD_BYTE=0)
  target_link_libraries(MadsMainDeps INTERFACE ws2_32 iphlpapi)
endif()
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES nlohmann_json::nlohmann_json)
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES cxxopts)
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES rang::rang rang)
link_first_available(
  MadsMainDeps
  LINK_SCOPE INTERFACE
  CANDIDATES
    tomlplusplus::tomlplusplus
    tomlplusplus::tomlplusplus_header_only
)
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES pugg::pugg pugg)
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES sodium sodium::sodium)
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES libzmq libzmq-static zmq)
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES zmqpp-static zmqpp)
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES inja::inja inja)
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES Snappy::snappy snappy)
if(MADS_ENABLE_MONGOCXX)
  link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES mongo::mongocxx_static mongocxx_static)
  link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES mongo::bsoncxx_static bsoncxx_static)
endif()
if(DEFINED _zmqpp_source_dir AND _zmqpp_source_dir)
  target_include_directories(
    MadsMainDeps
    INTERFACE
      "${_zmqpp_source_dir}/src"
  )
endif()
if(DEFINED _zmqpp_binary_dir AND _zmqpp_binary_dir)
  target_include_directories(
    MadsMainDeps
    INTERFACE
      "${_zmqpp_binary_dir}"
  )
endif()

if(MADS_BUILD_APPS)
  include(FetchContent)
  FetchContent_Populate(plugin 
    GIT_REPOSITORY https://github.com/pbosetti/mads_plugin.git
    GIT_TAG        v2.3-P7
    GIT_SHALLOW    TRUE
  )
  include_directories(
    ${plugin_SOURCE_DIR}/src
    ${nlohmann_json_SOURCE_DIR}/include
  )
  add_subdirectory(${SOURCE_DIR}/main)
  add_subdirectory(${SOURCE_DIR}/plugin)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/share/man)
endif()

#
#  ___           _        _ _
# |_ _|_ __  ___| |_ __ _| | |
#  | || '_ \/ __| __/ _` | | |
#  | || | | \__ \ || (_| | | |
# |___|_| |_|___/\__\__,_|_|_|
#
install(
  TARGETS ${LIB_NAME}
  EXPORT MadsTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  COMPONENT MadsCore
)

install(
  EXPORT MadsTargets
  NAMESPACE Mads::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Mads
  COMPONENT MadsCore
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/MadsConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/MadsConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Mads
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/MadsConfigVersion.cmake
  VERSION ${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}
  COMPATIBILITY SameMajorVersion
)
install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MadsConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MadsConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Mads
  COMPONENT MadsCore
)

file(GLOB LIB_HEADERS ${SOURCE_DIR}/*.hpp ${SOURCE_DIR}/*.h)
install(
  FILES ${LIB_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT MadsCore
)

# Rust plugin ABI header — lets C/C++ plugin wrappers include the vtable struct
if(NOT WIN32)
  install(
    FILES ${CMAKE_CURRENT_LIST_DIR}/src/main/mads_rust_plugin.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT MadsApps
  )

  # mads-plugin Rust crate source — installed so plugin authors can use a local
  # path dependency without network access when crates.io is unavailable.
  install(
    DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/rust/mads-plugin
    DESTINATION share/rust
    COMPONENT MadsApps
    PATTERN "target" EXCLUDE
  )
endif()

install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/share/images
  DESTINATION share
  COMPONENT MadsApps
)

install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/share/templates
  DESTINATION share
  COMPONENT MadsApps
)

# Data-driven plugin migration: definitions consumed by `mads plugin --update`,
# plus the dependency manifest that pins versions for freshly scaffolded plugins.
install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/share/plugin_migrations
  DESTINATION share
  COMPONENT MadsApps
)

install(FILES ${CMAKE_CURRENT_LIST_DIR}/share/plugin_deps.json
  DESTINATION share
  COMPONENT MadsApps
)

file(GLOB PYTHON_SCRIPTS ${CMAKE_CURRENT_LIST_DIR}/scripts/*.py)
install(PROGRAMS ${PYTHON_SCRIPTS}
  DESTINATION python
  COMPONENT MadsApps
)

# Isolated copy of the Python wrapper used by `mads setup-python`: a .pth file
# is written into the target environment pointing at this directory, so only
# mads_agent (and not the other helper scripts) becomes importable.
install(FILES ${CMAKE_CURRENT_LIST_DIR}/scripts/mads_agent.py
  DESTINATION share/mads-python
  COMPONENT MadsApps
)

if(UNIX)
  file(GLOB MADS_SHELL_SCRPTS ${CMAKE_CURRENT_LIST_DIR}/scripts/mads-*)
  install(PROGRAMS ${MADS_SHELL_SCRPTS}
    DESTINATION bin
    COMPONENT MadsApps
  )
endif()


#   ____            _               _             
#  |  _ \ __ _  ___| | ____ _  __ _(_)_ __   __ _ 
#  | |_) / _` |/ __| |/ / _` |/ _` | | '_ \ / _` |
#  |  __/ (_| | (__|   < (_| | (_| | | | | | (_| |
#  |_|   \__,_|\___|_|\_\__,_|\__, |_|_| |_|\__, |
#                             |___/         |___/ 

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${GIT_TAG}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_VENDOR "MADS Consortium")
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_SOURCE_DIR}/packages)
set(CPACK_STRIP_FILES TRUE)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
set(CPACK_PACKAGE_CONTACT "paolo.bosetti@unitn.it")
set(CMAKE_PROJECT_HOMEPAGE_URL "https://github.com/pbosetti/MADS")
set(CPACK_PACKAGE_DESCRIPTION "MADS installer: Multi-Agent Distributed System")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_LIST_DIR}/LICENSE")
if(APPLE)
  set(CPACK_GENERATOR "STGZ;TGZ;DragNDrop")
  if(CMAKE_OSX_ARCHITECTURES MATCHES ";")
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${GIT_TAG}-${CMAKE_SYSTEM_NAME}-universal")
  endif()
elseif(WIN32)
  include(CPackComponent)
  set(CPACK_GENERATOR "ZIP;INNOSETUP")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "MADS")
  set(CPACK_INNOSETUP_IGNORE_LICENSE_PAGE OFF)
  set(CPACK_INNOSETUP_IGNORE_README_PAGE ON)
  set(CPACK_INNOSETUP_SETUP_ChangesEnvironment ON)
  set(CPACK_INNOSETUP_EXTRA_SCRIPTS "${CMAKE_CURRENT_LIST_DIR}/iss/environment.iss")
  set(CPACK_INNOSETUP_CODE_FILES "${CMAKE_CURRENT_LIST_DIR}/iss/events.iss")
  set(CPACK_INSTALLED_DIRECTORIES 
    ${CMAKE_INSTALL_PREFIX}/lib ./lib
    ${CMAKE_INSTALL_PREFIX}/include ./include
  )
  cpack_add_component(SDK
    DISPLAY_NAME "Main SDK"
    HIDDEN
  )
  cpack_add_component(dev
    DISPLAY_NAME "dev"
    HIDDEN
  )
  cpack_add_component(Devel
    DISPLAY_NAME "Devel"
    HIDDEN
  )
  cpack_add_component(MadsCore
    DISPLAY_NAME "MADS core library"
    REQUIRED
  )
  cpack_add_component(MadsApps
    DISPLAY_NAME "MADS apps and executables"
  )
  

else()
  if(MADS_DIRECTOR)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libzstd1, libgnutls30, openssl, libsodium23, libxrandr2, libxinerama1, libxcursor1, libxi6, libxext6")
  else()
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libzstd1, libgnutls30, openssl")
  endif()
  set(CPACK_DEBIAN_PACKAGE_NAME MADS)
  set(CPACK_GENERATOR "DEB;TGZ")
endif()
include(CPack)
message(STATUS "Packaging enabled")
