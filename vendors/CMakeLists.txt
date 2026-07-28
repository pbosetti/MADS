#
#  _   _                _
# | | | | ___ _ __   __| | ___  _ __ ___
# | | | |/ _ \ '_ \ / _` |/ _ \| '__/ __|
# | |_| |  __/ | | | (_| | (_) | |  \__ \
#  \___/ \___|_| |_|\__,_|\___/|_|  |___/
#
include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED OFF)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1)
set(CMAKE_WARN_DEPRECATED OFF)

# Silence compiler warnings from third-party dependencies only.
if(MSVC)
  string(REGEX REPLACE "/W[3|4]" "/W0" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_compile_options(/wd4710 /wd4711 /wd4865 /wd5039 /wd4458 /wd4820 /wd4668 /wd4626 /wd4005 /wd4061 /wd4244 /wd4267 /wd4365 /wd4319 /wd4245)
else()
  add_compile_options(-w)
endif()

set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Build tests for dependencies that use BUILD_TESTS" FORCE)
set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)

# Dependency-specific static options where supported.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

set(CXXOPTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CXXOPTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CXXOPTS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

set(INJA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(INJA_INSTALL OFF CACHE BOOL "" FORCE)
set(INJA_EXPORT OFF CACHE BOOL "" FORCE)
set(INJA_USE_EMBEDDED_JSON OFF CACHE BOOL "" FORCE)

set(SNAPPY_INSTALL OFF CACHE BOOL "" FORCE)
set(ZMQPP_LIBZMQ_CMAKE ON CACHE BOOL "" FORCE)
set(ZMQPP_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZMQPP_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZMQPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZMQPP_BUILD_CLIENT OFF CACHE BOOL "" FORCE)
set(ZMQPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURVE ON CACHE BOOL "" FORCE)
set(WITH_LIBSODIUM ON CACHE BOOL "" FORCE)
set(WITH_LIBSODIUM_STATIC ON CACHE BOOL "" FORCE)
set(ENABLE_WS OFF CACHE BOOL "" FORCE)
set(WITH_TLS OFF CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_GnuTLS ON CACHE BOOL "" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
set(WITH_DOC OFF CACHE BOOL "" FORCE)
set(WITH_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CPACK OFF CACHE BOOL "" FORCE)

set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

# Snappy's compile-time SIMD feature probes are not reliable for universal
# macOS builds (x86_64+arm64): they may enable NEON globally and break x86_64.
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
  set(SNAPPY_HAVE_SSSE3 OFF CACHE BOOL "" FORCE)
  set(SNAPPY_HAVE_X86_CRC32 OFF CACHE BOOL "" FORCE)
  set(SNAPPY_HAVE_BMI2 OFF CACHE BOOL "" FORCE)
  set(SNAPPY_HAVE_NEON OFF CACHE BOOL "" FORCE)
  set(SNAPPY_HAVE_NEON_CRC32 OFF CACHE BOOL "" FORCE)
endif()

set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_UNINSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_MAN_PAGES OFF CACHE BOOL "" FORCE)
set(ENABLE_HTML_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_MAINTAINER_FLAGS OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE STRING "" FORCE)
set(ENABLE_STATIC ON CACHE STRING "" FORCE)
set(ENABLE_SASL OFF CACHE STRING "" FORCE)
set(ENABLE_ZSTD OFF CACHE STRING "" FORCE)
set(ENABLE_MONGODB_AWS_AUTH OFF CACHE STRING "" FORCE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
if(MADS_ENABLE_MONGOCXX)
  set(MONGOCXX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BSONCXX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
  cxxopts
  GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
  GIT_TAG v3.2.0
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  rang
  GIT_REPOSITORY https://github.com/agauniyal/rang.git
  GIT_TAG v3.2
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/pbosetti/tomlplusplus.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  pugg
  GIT_REPOSITORY https://github.com/pbosetti/pugg.git
  GIT_TAG 1.2.0
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  libsodium
  GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  libzmq
  GIT_REPOSITORY https://github.com/zeromq/libzmq.git
  GIT_TAG v4.3.5
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  zmqpp
  GIT_REPOSITORY https://github.com/zeromq/zmqpp.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  inja
  GIT_REPOSITORY https://github.com/pantor/inja.git
  GIT_TAG v3.4.0
  GIT_SHALLOW TRUE
)

set(GV2FSM_STATIC_ONLY ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  snappy
  GIT_REPOSITORY https://github.com/google/snappy.git
  GIT_TAG 1.2.2
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  gv2fsm
  GIT_REPOSITORY https://github.com/pbosetti/gv2fsm.git
  GIT_TAG v2.0.0
  GIT_SHALLOW TRUE
)

# reproc: cross-platform process spawning/pipes/timeouts, vendored for
# `mads-up` (src/up_supervisor.cpp). Plain C API only (REPROC++ OFF): the
# supervisor needs direct access to reproc_t::id (the child pid) to manage
# POSIX process groups itself (see up_supervisor.cpp), which the C++ wrapper
# does not expose.
set(REPROC++ OFF CACHE BOOL "" FORCE)
set(REPROC_TEST OFF CACHE BOOL "" FORCE)
set(REPROC_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  reproc
  GIT_REPOSITORY https://github.com/DaanDeMeyer/reproc.git
  GIT_TAG v9.0.0
  GIT_SHALLOW TRUE
)

if(MADS_ENABLE_MONGOCXX)
  set(MADS_MONGO_C_DRIVER_VERSION "2.3.3")
  set(MADS_MONGO_CXX_DRIVER_VERSION "4.4.0")

  # mongo-cxx-driver derives its own version from BUILD_VERSION. Left at the
  # default "0.0.0" it falls back to reading ${CMAKE_BINARY_DIR}/VERSION_CURRENT
  # -- a file it writes itself into *our* build tree -- and otherwise shells out
  # to a Python script. That makes the stamped version sticky across upgrades
  # (a build dir configured against r3.11.1 keeps reporting 3.11.1 forever) and
  # adds a Python dependency. Pin it to the tag we actually fetch.
  set(BUILD_VERSION "${MADS_MONGO_CXX_DRIVER_VERSION}" CACHE STRING "" FORCE)
  file(REMOVE "${CMAKE_BINARY_DIR}/VERSION_CURRENT")

  set(_mads_local_mongo_c_driver_src
      "${CMAKE_BINARY_DIR}/_deps/mongo-c-driver-src")
  if(EXISTS "${_mads_local_mongo_c_driver_src}/CMakeLists.txt")
    FetchContent_Declare(
      mongo_c_driver
      SOURCE_DIR "${_mads_local_mongo_c_driver_src}"
    )
  else()
    FetchContent_Declare(
      mongo_c_driver
      GIT_REPOSITORY https://github.com/mongodb/mongo-c-driver.git
      GIT_TAG ${MADS_MONGO_C_DRIVER_VERSION}
      GIT_SHALLOW TRUE
    )
  endif()
  unset(_mads_local_mongo_c_driver_src)

  FetchContent_Declare(
    mongo_cxx_driver
    GIT_REPOSITORY https://github.com/mongodb/mongo-cxx-driver.git
    GIT_TAG r${MADS_MONGO_CXX_DRIVER_VERSION}
    GIT_SHALLOW TRUE
  )
endif()

macro(add_vendor vendor_name)
  set(options)
  set(one_value_args EXCLUDE)
  set(multi_value_args)
  cmake_parse_arguments(ADD_VENDOR "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT DEFINED ADD_VENDOR_EXCLUDE)
    set(ADD_VENDOR_EXCLUDE NO)
  endif()

  # Accept common CMake booleans for EXCLUDE.
  if(ADD_VENDOR_EXCLUDE)
    set(_add_vendor_exclude_from_all TRUE)
  else()
    set(_add_vendor_exclude_from_all FALSE)
  endif()

  FetchContent_GetProperties(${vendor_name})
  if(NOT ${vendor_name}_POPULATED)
    FetchContent_Populate(${vendor_name})
    if(_add_vendor_exclude_from_all)
      set(_add_vendor_old_skip_install_rules "${CMAKE_SKIP_INSTALL_RULES}")
      set(CMAKE_SKIP_INSTALL_RULES ON)
      add_subdirectory(
        ${${vendor_name}_SOURCE_DIR}
        ${${vendor_name}_BINARY_DIR}
        EXCLUDE_FROM_ALL
      )
      if(DEFINED _add_vendor_old_skip_install_rules)
        set(CMAKE_SKIP_INSTALL_RULES "${_add_vendor_old_skip_install_rules}")
      else()
        unset(CMAKE_SKIP_INSTALL_RULES)
      endif()
    else()
      add_subdirectory(
        ${${vendor_name}_SOURCE_DIR}
        ${${vendor_name}_BINARY_DIR}
      )
    endif()
  endif()

  set(_vendor_source_dir "${${vendor_name}_SOURCE_DIR}")
  set(_vendor_binary_dir "${${vendor_name}_BINARY_DIR}")
  set(_add_vendor_install_public_headers TRUE)

  if(vendor_name STREQUAL "mongo_c_driver" OR vendor_name STREQUAL "mongo_cxx_driver")
    set(_add_vendor_install_public_headers FALSE)
  endif()

  # Install vendor headers into the main include prefix so installed public
  # Mads headers can resolve third-party includes.
  if(NOT _add_vendor_exclude_from_all AND _add_vendor_install_public_headers)
    if(EXISTS "${_vendor_source_dir}/include")
      install(
        DIRECTORY "${_vendor_source_dir}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT MadsCore
        FILES_MATCHING
          PATTERN "*.h"
          PATTERN "*.hh"
          PATTERN "*.hpp"
          PATTERN "*.hxx"
          PATTERN "*.inl"
          PATTERN "*.inc"
      )
    endif()

    if(EXISTS "${_vendor_source_dir}/src")
      install(
        DIRECTORY "${_vendor_source_dir}/src/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT MadsCore
        FILES_MATCHING
          PATTERN "*.h"
          PATTERN "*.hh"
          PATTERN "*.hpp"
          PATTERN "*.hxx"
          PATTERN "*.inl"
          PATTERN "*.inc"
      )
    endif()

    file(
      GLOB _vendor_root_headers
      "${_vendor_source_dir}/*.h"
      "${_vendor_source_dir}/*.hh"
      "${_vendor_source_dir}/*.hpp"
      "${_vendor_source_dir}/*.hxx"
      "${_vendor_source_dir}/*.inl"
      "${_vendor_source_dir}/*.inc"
    )
    if(_vendor_root_headers)
      install(
        FILES ${_vendor_root_headers}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT MadsCore
      )
    endif()

    file(
      GLOB _vendor_generated_headers
      "${_vendor_binary_dir}/*.h"
      "${_vendor_binary_dir}/*.hh"
      "${_vendor_binary_dir}/*.hpp"
      "${_vendor_binary_dir}/*.hxx"
      "${_vendor_binary_dir}/*.inl"
      "${_vendor_binary_dir}/*.inc"
    )
    if(_vendor_generated_headers)
      install(
        FILES ${_vendor_generated_headers}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT MadsCore
      )
    endif()
  endif()
endmacro()

macro(add_vendor_exclude_from_all vendor_name)
  add_vendor(${vendor_name} EXCLUDE YES)
endmacro()

function(prune_mongo_cxx_driver_installed_headers)
  install(CODE "
    set(_mads_install_root \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
    file(REMOVE_RECURSE
      \"\${_mads_install_root}/${CMAKE_INSTALL_INCLUDEDIR}/mongocxx/test\"
      \"\${_mads_install_root}/${CMAKE_INSTALL_INCLUDEDIR}/mongocxx/cmake\"
      \"\${_mads_install_root}/${CMAKE_INSTALL_INCLUDEDIR}/mongocxx/include\"
      \"\${_mads_install_root}/${CMAKE_INSTALL_INCLUDEDIR}/mongocxx/lib\"
      \"\${_mads_install_root}/${CMAKE_INSTALL_INCLUDEDIR}/bsoncxx/test\"
      \"\${_mads_install_root}/${CMAKE_INSTALL_INCLUDEDIR}/bsoncxx/cmake\"
      \"\${_mads_install_root}/${CMAKE_INSTALL_INCLUDEDIR}/bsoncxx/include\"
      \"\${_mads_install_root}/${CMAKE_INSTALL_INCLUDEDIR}/bsoncxx/third_party\"
      \"\${_mads_install_root}/${CMAKE_INSTALL_INCLUDEDIR}/bsoncxx/lib\"
    )
  " COMPONENT MadsCore)
endfunction()

add_vendor(nlohmann_json)
add_vendor(cxxopts)
add_vendor(rang)
add_vendor(tomlplusplus)
# gv2fsm also FetchContent's inja; configure it here first so gv2fsm reuses the
# existing target instead of reconfiguring inja with a conflicting json setup.
add_vendor_exclude_from_all(inja)
add_vendor(gv2fsm)
# gv2fsm installs its own `gv2fsm` executable, which is identical to our
# `mads-fsm`. We only want `mads-fsm` installed, so drop the redundant binary
# after gv2fsm's own install rules have run. (The static library and headers are
# still installed for SDK consumers.)
install(CODE "
  set(_mads_install_root \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
  file(REMOVE
    \"\${_mads_install_root}/${CMAKE_INSTALL_BINDIR}/gv2fsm\"
    \"\${_mads_install_root}/${CMAKE_INSTALL_BINDIR}/gv2fsm.exe\"
  )
")
add_vendor_exclude_from_all(pugg)
add_vendor_exclude_from_all(reproc)
add_vendor_exclude_from_all(libsodium)
set(SODIUM_INCLUDE_DIRS
    "${libsodium_SOURCE_DIR}/libsodium/src/libsodium/include"
    CACHE PATH "" FORCE)
if(WIN32)
  set(_mads_sodium_library "${libsodium_BINARY_DIR}/sodium.lib")
else()
  set(_mads_sodium_library "${libsodium_BINARY_DIR}/libsodium.a")
endif()
set(SODIUM_LIBRARIES
    "${_mads_sodium_library}"
    CACHE FILEPATH "" FORCE)
unset(_mads_sodium_library)
add_vendor(libzmq)
if(ANDROID)
  foreach(_mads_zmq_target libzmq libzmq-static)
    if(TARGET "${_mads_zmq_target}")
      get_target_property(_mads_zmq_link_libs "${_mads_zmq_target}" INTERFACE_LINK_LIBRARIES)
      if(_mads_zmq_link_libs)
        list(REMOVE_ITEM _mads_zmq_link_libs "-lpthreads")
        set_target_properties(
          "${_mads_zmq_target}"
          PROPERTIES INTERFACE_LINK_LIBRARIES "${_mads_zmq_link_libs}"
        )
      endif()
    endif()
  endforeach()
endif()
add_vendor(snappy)
if(TARGET snappy)
  install(
    TARGETS snappy
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT MadsCore
  )
endif()

# Work around zmqpp upstream CMake: generate_export_header(zmqpp) is called
# even when ZMQPP_BUILD_SHARED is OFF.
if(NOT TARGET zmqpp)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/zmqpp_dummy.cpp" "int zmqpp_dummy = 0;\n")
  add_library(zmqpp STATIC EXCLUDE_FROM_ALL "${CMAKE_CURRENT_BINARY_DIR}/zmqpp_dummy.cpp")
endif()

# zmqpp has legacy cmake_minimum_required(2.8.12) and links to parent-defined
# target "zmqpp" on Windows; force modern policy behavior in the subdir.
set(CMAKE_POLICY_DEFAULT_CMP0079 NEW)
# zmqpp unconditionally searches for zmq.h; point it to fetched libzmq headers.
set(ZEROMQ_INCLUDE_DIR "${libzmq_SOURCE_DIR}/include" CACHE PATH "" FORCE)
add_vendor_exclude_from_all(zmqpp)
if(EXISTS "${zmqpp_SOURCE_DIR}/src/zmqpp")
  install(
    DIRECTORY "${zmqpp_SOURCE_DIR}/src/zmqpp/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/zmqpp"
    COMPONENT MadsCore
    FILES_MATCHING
      PATTERN "*.h"
      PATTERN "*.hh"
      PATTERN "*.hpp"
      PATTERN "*.hxx"
      PATTERN "*.inl"
      PATTERN "*.inc"
  )
endif()
if(EXISTS "${zmqpp_BINARY_DIR}/zmqpp_export.h")
  install(
    FILES "${zmqpp_BINARY_DIR}/zmqpp_export.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/zmqpp"
    COMPONENT MadsCore
  )
endif()
if(MSVC)
  # zmqpp expects SOCKET to be available on Windows.
  if(TARGET zmqpp-static)
    target_compile_definitions(zmqpp-static PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(zmqpp-static PRIVATE /FIwinsock2.h)
  endif()
  if(TARGET zmqpp)
    target_compile_definitions(zmqpp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(zmqpp PRIVATE /FIwinsock2.h)
  endif()
endif()

if(MADS_ENABLE_MONGOCXX)
  # Force mongo-cxx-driver to bind to in-tree libmongoc/libbson targets instead
  # of stale user-local packages cached in the build directory.
  set(ENABLE_EXTRA_ALIGNMENT OFF CACHE BOOL "" FORCE)
  add_vendor(mongo_c_driver)

  # Ensure mongo-c-driver (used by mongo_cxx_driver) uses fetched Snappy.
  set(ENABLE_SNAPPY ON CACHE STRING "" FORCE)
  set(SNAPPY_INCLUDE_DIRS "${snappy_SOURCE_DIR}" CACHE PATH "" FORCE)
  set(SNAPPY_LIBRARIES "$<TARGET_FILE:snappy>" CACHE STRING "" FORCE)

  add_vendor(mongo_cxx_driver)
  prune_mongo_cxx_driver_installed_headers()
endif()
