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

# reproc needs sigtimedwait(), which Bionic only has from API 23. Without
# ANDROID_PLATFORM the NDK silently picks its minimum (21), and the build would
# fail deep inside reproc: catch it here. The legacy NDK toolchain file (the
# default) sets ANDROID_PLATFORM_LEVEL, the new one CMAKE_SYSTEM_VERSION.
if(ANDROID)
  set(_mads_android_api "${ANDROID_PLATFORM_LEVEL}")
  if(NOT _mads_android_api)
    set(_mads_android_api "${CMAKE_SYSTEM_VERSION}")
  endif()
  if(_mads_android_api LESS 23)
    message(FATAL_ERROR "Android API level ${_mads_android_api} is too old: MADS needs 23 or later. "
      "Pass -DANDROID_PLATFORM=android-24 (and -DANDROID_ABI=<abi>) when configuring; "
      "if they were lost, the cache was recreated without them.")
  endif()
  unset(_mads_android_api)
endif()

include(GNUInstallDirs)
option(MADS_ENABLE_MONGOCXX "Enable MongoDB C++ driver dependency" ON)
option(MADS_BUILD_APPS "Build executables" ${PROJECT_IS_TOP_LEVEL})
if(ANDROID)
  set(_mads_gui_default OFF)
else()
  set(_mads_gui_default ON)
endif()
option(MADS_ENABLE_GUI "Build the GUI apps (mads-chat, mads-director); requires a graphical interface" ${_mads_gui_default})
# One-shot migration of the former MADS_DIRECTOR option in existing build dirs.
if(DEFINED CACHE{MADS_DIRECTOR})
  message(DEPRECATION "MADS_DIRECTOR is deprecated, use MADS_ENABLE_GUI instead (migrating value ${MADS_DIRECTOR})")
  set(MADS_ENABLE_GUI ${MADS_DIRECTOR} CACHE BOOL "Build the GUI apps (mads-chat, mads-director); requires a graphical interface" FORCE)
  unset(MADS_DIRECTOR CACHE)
endif()
option(MADS_BUILD_TESTS "Build unit tests (Catch2)" OFF)
option(MADS_COVERAGE "Instrument MadsCore and tests for coverage analysis" OFF)

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

# mongo_fetch.cpp unconditionally includes <mongocxx/...>, so it can only be
# part of the library when the MongoDB driver is actually vendored in.
# (logger.cpp guards its driver use with MADS_HAS_MONGOCXX and still builds as a
# file-only logger, so it stays in the library either way.)
if(NOT MADS_ENABLE_MONGOCXX)
  list(REMOVE_ITEM LIB_SOURCES ${SOURCE_DIR}/mongo_fetch.cpp)
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
# reproc is a private implementation detail of src/up_supervisor.cpp (mads-up
# process spawning): no installed header depends on it, so it is not part of
# MadsMainDeps either.
link_first_available(${LIB_NAME} CANDIDATES reproc)
link_first_available(${LIB_NAME} CANDIDATES sodium sodium::sodium)
link_first_available(${LIB_NAME} CANDIDATES libzmq libzmq-static zmq)
# cppzmq is header-only; vendors/ populates it without configuring its project.
if(MADS_CPPZMQ_INCLUDE_DIR)
  target_include_directories(${LIB_NAME} PRIVATE "${MADS_CPPZMQ_INCLUDE_DIR}")
endif()
link_first_available(${LIB_NAME} CANDIDATES inja::inja inja)
link_first_available(${LIB_NAME} CANDIDATES Snappy::snappy snappy)
if(MADS_ENABLE_MONGOCXX)
  # Guards the driver-dependent code paths in logger.cpp. PRIVATE on purpose:
  # no installed header depends on it, so SDK consumers never need it. Use
  # Mads::Logger::has_mongo_support() to query the build at runtime instead.
  target_compile_definitions(${LIB_NAME} PRIVATE MADS_HAS_MONGOCXX)
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

if(MADS_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Instrument only MadsCore (PRIVATE flags): vendored dependencies stay out
    # of the report and build fast. Atomic profile updates are required because
    # Agent spawns drain/remote-control/watchdog threads.
    target_compile_options(${LIB_NAME} PRIVATE -O0 -g --coverage -fprofile-update=atomic)
    target_link_options(${LIB_NAME} PRIVATE --coverage)
    # Counters recorded against the previous build of this library no longer
    # match its arc layout, so drop them whenever it is rebuilt. Without this,
    # editing an instrumented source floods every later run with
    # "cannot merge previous GCDA file: corrupt arc tag" (one line per arc) and
    # leaves the report mixing counts from two different builds.
    add_custom_command(
      TARGET ${LIB_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND}
              -D MADS_COVERAGE_DIR=${CMAKE_BINARY_DIR}
              -P ${CMAKE_CURRENT_LIST_DIR}/cmake/ResetCoverageData.cmake
      COMMENT "Discarding coverage data from the previous build"
      VERBATIM
    )
  else()
    message(WARNING "MADS_COVERAGE requires GCC or Clang; coverage flags not applied")
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
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES inja::inja inja)
link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES Snappy::snappy snappy)
if(MADS_ENABLE_MONGOCXX)
  link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES mongo::mongocxx_static mongocxx_static)
  link_first_available(MadsMainDeps LINK_SCOPE INTERFACE CANDIDATES mongo::bsoncxx_static bsoncxx_static)
endif()
if(MADS_CPPZMQ_INCLUDE_DIR)
  target_include_directories(
    MadsMainDeps
    INTERFACE
      "${MADS_CPPZMQ_INCLUDE_DIR}"
  )
endif()

#
#  _____         _
# |_   _|__  ___| |_ ___
#   | |/ _ \/ __| __/ __|
#   | |  __/\__ \ |_\__ \
#   |_|\___||___/\__|___/
#
# Note: enable_testing() is used directly (not include(CTest)) because
# vendors/CMakeLists.txt forces BUILD_TESTING OFF in the cache to keep
# third-party test suites out of the build.
if(MADS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MADS_COVERAGE)
  # Manual reset, for when a run is abandoned part-way and the leftover counters
  # should not feed the next report.
  add_custom_target(coverage-reset
    COMMAND ${CMAKE_COMMAND}
            -D MADS_COVERAGE_DIR=${CMAKE_BINARY_DIR}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/ResetCoverageData.cmake
    COMMENT "Discarding accumulated coverage data"
    VERBATIM
  )

  find_program(GCOVR_EXECUTABLE gcovr)
  if(GCOVR_EXECUTABLE)
    # clang emits gcov-compatible data but needs its own reader tool.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set(MADS_GCOV_TOOL --gcov-executable "llvm-cov gcov")
    else()
      set(MADS_GCOV_TOOL)
    endif()
    add_custom_target(coverage
      # Start from a clean slate so the report reflects this test run only, and
      # not the incidental execution from catch_discover_tests enumerating each
      # test binary at build time.
      COMMAND ${CMAKE_COMMAND}
              -D MADS_COVERAGE_DIR=${CMAKE_BINARY_DIR}
              -P ${CMAKE_CURRENT_LIST_DIR}/cmake/ResetCoverageData.cmake
      COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
      COMMAND ${GCOVR_EXECUTABLE} --config ${CMAKE_SOURCE_DIR}/gcovr.cfg
              --object-directory ${CMAKE_BINARY_DIR} ${MADS_GCOV_TOOL}
              --html-details ${CMAKE_BINARY_DIR}/coverage_html/index.html
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Running tests and generating coverage report"
      VERBATIM
    )
  else()
    message(WARNING "gcovr not found: the 'coverage' target will not be available")
  endif()
endif()

if(MADS_BUILD_APPS)
  include(FetchContent)
  FetchContent_Populate(plugin 
    GIT_REPOSITORY https://github.com/pbosetti/mads_plugin.git
    GIT_TAG        v2.5-P8
    GIT_SHALLOW    TRUE
  )
  include_directories(
    ${plugin_SOURCE_DIR}/src
    ${nlohmann_json_SOURCE_DIR}/include
  )
  add_subdirectory(${SOURCE_DIR}/main)
  add_subdirectory(${SOURCE_DIR}/plugin)
  if(MADS_ENABLE_GUI)
    add_subdirectory(${SOURCE_DIR}/gui)
  endif()
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/share/man)
endif()

#
#  ___           _        _ _
# |_ _|_ __  ___| |_ __ _| | |
#  | || '_ \/ __| __/ _` | | |
#  | || | | \__ \ || (_| | | |
# |___|_| |_|___/\__\__,_|_|_|
#
# COMPONENT binds to the artifact kind it follows, so it is repeated for each:
# otherwise the Windows DLL (a RUNTIME artifact) lands in "Unspecified".
install(
  TARGETS ${LIB_NAME}
  EXPORT MadsTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT MadsCore
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT MadsCore
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT MadsCore
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

# Note: this glob is deliberately non-recursive, so internal headers under
# src/detail/ are not part of the installed SDK.
file(GLOB LIB_HEADERS ${SOURCE_DIR}/*.hpp ${SOURCE_DIR}/*.h)
# MongoFetch is only compiled into the library when the driver is vendored in;
# shipping its header otherwise would just hand consumers link errors.
if(NOT MADS_ENABLE_MONGOCXX)
  list(REMOVE_ITEM LIB_HEADERS ${SOURCE_DIR}/mongo_fetch.hpp)
endif()
install(
  FILES ${LIB_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT MadsCore
)

# Data and scripts for the command line utilities (and the GUI apps): useless,
# and not installed, in a library-only build (MADS_BUILD_APPS=OFF, e.g. Android).
if(MADS_BUILD_APPS)
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

  # Used by both the command line utilities and the GUI apps.
  install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/share/images
    DESTINATION share
    COMPONENT MadsCore
  )

  install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/share/templates
    DESTINATION share
    COMPONENT MadsApps
  )

  # Agent skill scaffolded into new plugin projects by `mads plugin` (and
  # refreshed by `mads plugin --update`): the runtime context a plugin author's
  # AI assistant needs but cannot read out of the plugin API.
  install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/share/skills
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
  # Only these components go into the installer: anything else (e.g. a
  # dependency's own "Unspecified" rules) is deliberately left out.
  set(CPACK_COMPONENTS_ALL MadsCore MadsApps)
  # Inno Setup preselects the components of the first install type: "Full"
  # makes the optional components selected by default.
  cpack_add_install_type(Full DISPLAY_NAME "Full installation")
  cpack_add_component(MadsCore
    DISPLAY_NAME "Core library"
    DESCRIPTION "libMadsCore, its runtime dependencies and the C++ headers"
    REQUIRED
    INSTALL_TYPES Full
  )
  cpack_add_component(MadsApps
    DISPLAY_NAME "Command line utilities"
    DESCRIPTION "mads and all the mads-* command line tools, plugins and templates"
    DEPENDS MadsCore
    INSTALL_TYPES Full
  )
  if(MADS_ENABLE_GUI)
    list(APPEND CPACK_COMPONENTS_ALL MadsGui)
    cpack_add_component(MadsGui
      DISPLAY_NAME "GUI components"
      DESCRIPTION "The mads-director and mads-chat apps"
      DEPENDS MadsCore
      INSTALL_TYPES Full
    )
  endif()

elseif(ANDROID)
  # Cross build: a plain SDK archive (lib/, include/) for the target ABI. The
  # Linux branch below would build a .deb, which makes no sense here and needs
  # dpkg tools the (usually Windows or macOS) host lacks.
  set(CPACK_GENERATOR "TGZ")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/")
  set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${GIT_TAG}-Android-${CMAKE_ANDROID_ARCH_ABI}")

else()
  # Runtime dependencies are computed by dpkg-shlibdeps from the binaries
  # actually packaged (vendored libraries such as libsodium/libzmq are linked
  # statically, the system ones are OpenSSL, libc/libstdc++ and, with the GUI,
  # X11/GL), so the GUI and headless packages each get exactly what they need.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  if(MADS_ENABLE_GUI)
    # GLFW loads the X11 libraries with dlopen(), invisible to dpkg-shlibdeps:
    # declare them explicitly (CPack merges these with the computed ones).
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libx11-6, libxrandr2, libxinerama1, libxcursor1, libxi6, libxext6")
    # mads-chat file dialogs (portable-file-dialogs) shell out to zenity at
    # runtime, which dpkg-shlibdeps cannot see.
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "zenity")
  else()
    # GUI-less variant (no mads-chat/mads-director, no X11/GL deps): CI packages
    # it from the same build tree, reconfigured with MADS_ENABLE_GUI=OFF.
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}_headless")
  endif()
  set(CPACK_DEBIAN_PACKAGE_NAME MADS)
  set(CPACK_GENERATOR "DEB;TGZ")
endif()
include(CPack)
message(STATUS "Packaging enabled")
