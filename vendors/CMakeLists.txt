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
# libzmq must be a DLL on Windows, and only on Windows. A static libzmq gets
# linked into MadsCore.dll *and*, separately, into every executable that also
# touches the ZMQ API directly (the test binaries, mads, mads-broker,
# mads-bridge, mads-dealer, mads-worker). PE gives each module its own private
# copy of the library's globals, so such a process runs two libzmq instances --
# including two copies of bundled wepoll's epoll handle table. A handle created
# through one copy is then invisible to the other, and epoll_ctl fails with
# EINVAL ("handle not in tree"), which libzmq turns into an errno_assert abort.
# ELF and Mach-O collapse the duplicate definitions onto one, so Unix never had
# the problem and keeps the static build (one fewer artifact to ship).
if(WIN32)
  set(BUILD_SHARED ON CACHE BOOL "" FORCE)
  set(BUILD_STATIC OFF CACHE BOOL "" FORCE)
else()
  set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC ON CACHE BOOL "" FORCE)
endif()
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
  cppzmq
  GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
  GIT_TAG v4.11.0
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

# Configures a FetchContent dependency. Its own install rules are skipped:
# MADS links every vendored library into MadsCore (or uses it at build time
# only), so the package must not ship their static archives, CMake/pkg-config
# files or private headers. The few headers that installed MADS headers include
# are installed explicitly below (see "Public third-party headers").
# The subdirectory is added EXCLUDE_FROM_ALL: its targets are still built when
# something depends on them (MadsCore, mads-fsm), and CMake leaves such
# directories out of the parent's install script, which would otherwise
# include() a cmake_install.cmake that CMAKE_SKIP_INSTALL_RULES never wrote.
#   KEEP_INSTALL_RULES YES keep the dependency's own install rules (and ALL)
macro(add_vendor vendor_name)
  set(options)
  set(one_value_args KEEP_INSTALL_RULES)
  set(multi_value_args)
  cmake_parse_arguments(ADD_VENDOR "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  FetchContent_GetProperties(${vendor_name})
  if(NOT ${vendor_name}_POPULATED)
    FetchContent_Populate(${vendor_name})
    set(_add_vendor_old_skip_install_rules "${CMAKE_SKIP_INSTALL_RULES}")
    if(ADD_VENDOR_KEEP_INSTALL_RULES)
      set(_add_vendor_exclude_from_all)
    else()
      set(_add_vendor_exclude_from_all EXCLUDE_FROM_ALL)
      set(CMAKE_SKIP_INSTALL_RULES ON)
    endif()
    add_subdirectory(
      ${${vendor_name}_SOURCE_DIR}
      ${${vendor_name}_BINARY_DIR}
      ${_add_vendor_exclude_from_all}
    )
    set(CMAKE_SKIP_INSTALL_RULES "${_add_vendor_old_skip_install_rules}")
    unset(_add_vendor_old_skip_install_rules)
    unset(_add_vendor_exclude_from_all)
  endif()
endmacro()

# Kept for readability at the call sites: every vendor is now EXCLUDE_FROM_ALL.
macro(add_vendor_exclude_from_all vendor_name)
  add_vendor(${vendor_name})
endmacro()


add_vendor(nlohmann_json)
add_vendor(cxxopts)
add_vendor(rang)
add_vendor(tomlplusplus)
# gv2fsm also FetchContent's inja; configure it here first so gv2fsm reuses the
# existing target instead of reconfiguring inja with a conflicting json setup.
add_vendor_exclude_from_all(inja)
# Build-time only (mads-fsm): nothing of gv2fsm or its tree-sitter is installed.
add_vendor(gv2fsm)
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
if(WIN32)
  # libzmq is a DLL on Windows (see BUILD_SHARED above): keep its own install
  # rules there, so libzmq.dll, its import library and CMake files ship.
  add_vendor(libzmq KEEP_INSTALL_RULES YES)
else()
  add_vendor(libzmq)
endif()
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
if(WIN32 AND TARGET libzmq)
  # Emit libzmq.dll into the same directory as MadsCore.dll rather than leaving
  # it under _deps/libzmq-build. Everything that has to find it at run time --
  # the ctest DL_PATHS entry, a developer running an executable out of the build
  # tree -- then needs one directory on PATH instead of two.
  set_target_properties(libzmq PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
# cppzmq is header-only (zmq.hpp + zmq_addon.hpp). Its own CMake project insists
# on find_package(ZeroMQ), which would not see the in-tree libzmq target above,
# so populate the sources and expose the headers directly instead of configuring
# it. MADS_CPPZMQ_INCLUDE_DIR is consumed by the top-level CMakeLists.
FetchContent_GetProperties(cppzmq)
if(NOT cppzmq_POPULATED)
  FetchContent_Populate(cppzmq)
endif()
set(MADS_CPPZMQ_INCLUDE_DIR "${cppzmq_SOURCE_DIR}" CACHE PATH "" FORCE)
install(
  FILES
    "${cppzmq_SOURCE_DIR}/zmq.hpp"
    "${cppzmq_SOURCE_DIR}/zmq_addon.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  COMPONENT MadsCore
)

add_vendor(snappy)  # statically linked into MadsCore, nothing installed

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
endif()

#
# Public third-party headers
#
# Only what the installed MADS headers include (verified by preprocessing each
# of them against the installed include dir): libzmq's zmq.h, cppzmq (above),
# nlohmann/json, toml++, cxxopts and rang. Everything else is linked into
# MadsCore or used at build time only.
install(DIRECTORY "${nlohmann_json_SOURCE_DIR}/include/nlohmann"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT MadsCore)
install(DIRECTORY "${tomlplusplus_SOURCE_DIR}/include/toml++"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT MadsCore)
install(FILES
    "${cxxopts_SOURCE_DIR}/include/cxxopts.hpp"
    "${rang_SOURCE_DIR}/include/rang.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT MadsCore)
if(NOT WIN32)
  # On Windows libzmq's own install rules ship zmq.h (and its license).
  install(FILES "${libzmq_SOURCE_DIR}/include/zmq.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT MadsCore)
  install(FILES "${libzmq_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/zmq" RENAME LICENSE.txt COMPONENT MadsCore)
endif()

# License/notice files of libraries statically linked into the shipped binaries.
if(MADS_ENABLE_MONGOCXX)
  install(FILES
      "${mongo_c_driver_SOURCE_DIR}/COPYING"
      "${mongo_c_driver_SOURCE_DIR}/THIRD_PARTY_NOTICES"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mongo-c-driver" COMPONENT MadsCore)
  install(FILES
      "${mongo_cxx_driver_SOURCE_DIR}/LICENSE"
      "${mongo_cxx_driver_SOURCE_DIR}/THIRD-PARTY-NOTICES"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mongo-cxx-driver" COMPONENT MadsCore)
endif()
