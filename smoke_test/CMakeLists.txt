#   ____                  _          _            _
#  / ___| _ __ ___   ___ | | _____  | |_ ___  ___| |_ ___
#  \___ \| '_ ` _ \ / _ \| |/ / _ \ | __/ _ \/ __| __/ __|
#   ___) | | | | | | (_) |   <  __/ | ||  __/\__ \ |_\__ \
#  |____/|_| |_| |_|\___/|_|\_\___|  \__\___||___/\__|___/
#
# MADS Smoke Test Suite
# Standalone CMake project that validates an installed MADS distribution.
# Usage:
#   cmake -B build -S . -DCMAKE_PREFIX_PATH=/path/to/mads/install
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
cmake_minimum_required(VERSION 3.22)
project(MadsSmokeTest VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find the installed MADS package
find_package(Mads REQUIRED)
message(STATUS "Found Mads: ${Mads_DIR}")

# Derive MADS_PREFIX from the Mads_DIR (which is <prefix>/lib/cmake/Mads)
get_filename_component(MADS_PREFIX "${Mads_DIR}/../../.." ABSOLUTE)
message(STATUS "MADS prefix: ${MADS_PREFIX}")

# Determine platform-specific library suffix and plugin directory
if(APPLE)
  set(MADS_LIB_SUFFIX ".dylib")
  set(MADS_PLUGIN_DIR "${MADS_PREFIX}/lib")
elseif(WIN32)
  set(MADS_LIB_SUFFIX ".dll")
  set(MADS_PLUGIN_DIR "${MADS_PREFIX}/bin")
else()
  set(MADS_LIB_SUFFIX ".so")
  set(MADS_PLUGIN_DIR "${MADS_PREFIX}/lib")
endif()
if(NOT WIN32)
  set(MADS_LIB_PREFIX "lib")
endif()

set(MADS_BIN_DIR "${MADS_PREFIX}/bin")
set(MADS_PYTHON_DIR "${MADS_PREFIX}/python")
if(WIN32)
  set(MADS_LIB_DIR "${MADS_PREFIX}/bin")
else()
  set(MADS_LIB_DIR "${MADS_PREFIX}/lib")
endif()
enable_testing()
include(CTest)

if(CMAKE_CONFIGURATION_TYPES)
  set(SMOKE_TEST_BUILD_CONFIG_ARGS --config $<CONFIG>)
else()
  set(SMOKE_TEST_BUILD_CONFIG_ARGS)
endif()

# Find Python for wrapper tests
find_package(Python3 COMPONENTS Interpreter)

# Config file for broker-dependent tests
set(SMOKE_INI "${CMAKE_CURRENT_SOURCE_DIR}/mads_smoke.ini")

# ============================================================================
# Phase 1: Executable existence tests (no broker needed)
# ============================================================================
set(MADS_EXECUTABLES
  mads
  mads-broker
  mads-feedback
  mads-source
  mads-filter
  mads-sink
  mads-logger
  mads-dealer
  mads-worker
)

foreach(exe IN LISTS MADS_EXECUTABLES)
  add_test(
    NAME exe_help_${exe}
    COMMAND "${MADS_BIN_DIR}/${exe}" --help
  )
  set_tests_properties(exe_help_${exe} PROPERTIES
    TIMEOUT 5
    LABELS "executable;no_broker"
  )
endforeach()

# Also test the plugin maker
add_test(
  NAME exe_help_mads-plugin
  COMMAND "${MADS_BIN_DIR}/mads-plugin" --help
)
set_tests_properties(exe_help_mads-plugin PROPERTIES
  TIMEOUT 5
  LABELS "executable;no_broker"
)

# ============================================================================
# Phase 2: Plugin file existence tests (no broker needed)
# ============================================================================
set(MADS_PLUGINS
  publish.plugin
  bridge.plugin
  feedback.plugin
  monitor.plugin
)

foreach(plugin IN LISTS MADS_PLUGINS)
  if(WIN32)
    add_test(
      NAME plugin_exists_${plugin}
      COMMAND ${CMAKE_COMMAND} -E cat "${MADS_PLUGIN_DIR}/${plugin}"
    )
  else()
    add_test(
      NAME plugin_exists_${plugin}
      COMMAND test -f "${MADS_PLUGIN_DIR}/${plugin}"
    )
  endif()
  set_tests_properties(plugin_exists_${plugin} PROPERTIES
    TIMEOUT 5
    LABELS "plugin;no_broker"
  )
endforeach()

# ============================================================================
# Phase 3: Plugin stub generation test (no broker needed)
# ============================================================================
set(STUB_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/stub_gen_test")
add_test(
  NAME plugin_stub_generate
  COMMAND "${MADS_BIN_DIR}/mads-plugin" smoke_gen -t source -d "${STUB_GEN_DIR}"
)
set_tests_properties(plugin_stub_generate PROPERTIES
  TIMEOUT 15
  LABELS "plugin_gen;no_broker"
)

add_test(
  NAME plugin_stub_files_exist
  COMMAND ${CMAKE_COMMAND} -E cat
    "${STUB_GEN_DIR}/CMakeLists.txt"
    "${STUB_GEN_DIR}/src/smoke_gen.cpp"
)
set_tests_properties(plugin_stub_files_exist PROPERTIES
  TIMEOUT 5
  DEPENDS plugin_stub_generate
  LABELS "plugin_gen;no_broker"
)

add_test(
  NAME plugin_stub_configure
  COMMAND ${CMAKE_COMMAND}
    -B "${STUB_GEN_DIR}/build"
    -S "${STUB_GEN_DIR}"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
)
set_tests_properties(plugin_stub_configure PROPERTIES
  TIMEOUT 600
  DEPENDS plugin_stub_files_exist
  LABELS "plugin_gen;no_broker"
)

add_test(
  NAME plugin_stub_build
  COMMAND ${CMAKE_COMMAND} --build "${STUB_GEN_DIR}/build" ${SMOKE_TEST_BUILD_CONFIG_ARGS}
)
set_tests_properties(plugin_stub_build PROPERTIES
  TIMEOUT 300
  DEPENDS plugin_stub_configure
  LABELS "plugin_gen;no_broker"
)


# ============================================================================
# Phase 4: Compiled test targets
# ============================================================================

# C++ custom agent test (compile + link against Mads::Mads)
add_executable(test_cpp_agent src/test_cpp_agent.cpp)
target_link_libraries(test_cpp_agent PRIVATE Mads::Mads)

# C wrapper test (C linkage against Mads::Mads)
add_executable(test_c_agent src/test_c_agent.c)
target_link_libraries(test_c_agent PRIVATE Mads::Mads)

# C++ messaging test (blocking, non-blocking, queue sizes, timeout)
add_executable(test_messaging src/test_messaging.cpp)
target_link_libraries(test_messaging PRIVATE Mads::Mads)

# Compile-only tests (no broker needed, just verify linkage)
add_test(
  NAME compile_cpp_agent
  COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" ${SMOKE_TEST_BUILD_CONFIG_ARGS} --target test_cpp_agent
)
set_tests_properties(compile_cpp_agent PROPERTIES
  TIMEOUT 30
  LABELS "compile;no_broker"
)

add_test(
  NAME compile_c_agent
  COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" ${SMOKE_TEST_BUILD_CONFIG_ARGS} --target test_c_agent
)
set_tests_properties(compile_c_agent PROPERTIES
  TIMEOUT 30
  LABELS "compile;no_broker"
)

add_test(
  NAME compile_messaging
  COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" ${SMOKE_TEST_BUILD_CONFIG_ARGS} --target test_messaging
)
set_tests_properties(compile_messaging PROPERTIES
  TIMEOUT 30
  LABELS "compile;no_broker"
)

# ============================================================================
# Phase 5: Runtime tests (broker required — run via run_smoke_tests.py)
# ============================================================================

# C++ agent: create, init, connect, publish, disconnect
add_test(
  NAME run_cpp_agent
  COMMAND test_cpp_agent tcp://localhost:19092
)
set_tests_properties(run_cpp_agent PROPERTIES
  TIMEOUT 15
  LABELS "runtime;broker_required"
  DEPENDS compile_cpp_agent
)

# C agent: create, init, publish, receive non-blocking, disconnect
add_test(
  NAME run_c_agent
  COMMAND test_c_agent tcp://localhost:19092
)
set_tests_properties(run_c_agent PROPERTIES
  TIMEOUT 15
  LABELS "runtime;broker_required"
  DEPENDS compile_c_agent
)

# Messaging tests: each test case is a separate CTest entry
set(MESSAGING_TESTS
  nonblocking
  blocking
  lkv
  queue
  timeout
)

foreach(tc IN LISTS MESSAGING_TESTS)
  add_test(
    NAME messaging_${tc}
    COMMAND test_messaging --test-case ${tc} --settings-uri tcp://localhost:19092
  )
  set_tests_properties(messaging_${tc} PROPERTIES
    TIMEOUT 20
    LABELS "runtime;broker_required;messaging"
    DEPENDS compile_messaging
  )
endforeach()

# Plugin loading runtime tests (start plugin, verify it runs, then terminate)
set(PLUGIN_TEST_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_plugin_test.py")

add_test(
  NAME plugin_run_source
  COMMAND ${Python3_EXECUTABLE} "${PLUGIN_TEST_SCRIPT}"
    "${MADS_BIN_DIR}/mads-source"
    "${MADS_PLUGIN_DIR}/publish.plugin"
    -s tcp://localhost:19092
)
set_tests_properties(plugin_run_source PROPERTIES
  TIMEOUT 15
  LABELS "runtime;broker_required;plugin"
)

add_test(
  NAME plugin_run_filter
  COMMAND ${Python3_EXECUTABLE} "${PLUGIN_TEST_SCRIPT}"
    "${MADS_BIN_DIR}/mads-filter"
    "${MADS_PLUGIN_DIR}/bridge.plugin"
    -s tcp://localhost:19092
)
set_tests_properties(plugin_run_filter PROPERTIES
  TIMEOUT 15
  LABELS "runtime;broker_required;plugin"
)

add_test(
  NAME plugin_run_sink
  COMMAND ${Python3_EXECUTABLE} "${PLUGIN_TEST_SCRIPT}"
    "${MADS_BIN_DIR}/mads-sink"
    "${MADS_PLUGIN_DIR}/feedback.plugin"
    -s tcp://localhost:19092
)
set_tests_properties(plugin_run_sink PROPERTIES
  TIMEOUT 15
  LABELS "runtime;broker_required;plugin"
)

# ============================================================================
# Phase 6: Python wrapper test (broker required)
# ============================================================================
if(Python3_FOUND)
  add_test(
    NAME python_agent
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_python_agent.py"
      --settings-uri tcp://localhost:19092
  )
  set_tests_properties(python_agent PROPERTIES
    TIMEOUT 15
    LABELS "runtime;broker_required;python"
    ENVIRONMENT "MADS_LIB_PATH=${MADS_LIB_DIR}/${MADS_LIB_PREFIX}MadsCore${MADS_LIB_SUFFIX};PYTHONPATH=${MADS_PYTHON_DIR}"
  )
else()
  message(WARNING "Python3 not found — skipping Python wrapper tests")
endif()
