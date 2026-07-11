#
#  _   _       _ _     _____         _
# | | | |_ __ (_) |_  |_   _|__  ___| |_ ___
# | | | | '_ \| | __|   | |/ _ \/ __| __/ __|
# | |_| | | | | | |_    | |  __/\__ \ |_\__ \
#  \___/|_| |_|_|\__|   |_|\___||___/\__|___/
#
# Catch2 is a test-only dependency: it is fetched here rather than in
# vendors/ so that it never enters the install/package set.
include(FetchContent)
FetchContent_Declare(catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_library(mads_test_flags INTERFACE)
if(MADS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mads_test_flags INTERFACE -O0 -g --coverage -fprofile-update=atomic)
  target_link_options(mads_test_flags INTERFACE --coverage)
endif()

# MadsCore leaves OpenSSL symbols (https_client) undefined on Linux; the
# executables in src/main link ssl/crypto themselves, and so must the tests.
if(UNIX AND NOT APPLE)
  set(MADS_TEST_SSL_LIBS ssl crypto)
endif()

# One test executable per test_*.cpp file: new suites are added by dropping a
# file in this directory, with no CMake edits required.
file(GLOB MADS_TEST_SOURCES ${CMAKE_CURRENT_LIST_DIR}/test_*.cpp)
foreach(test_src IN LISTS MADS_TEST_SOURCES)
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE
    Mads::Mads
    MadsMainDeps
    Catch2::Catch2WithMain
    mads_test_flags
    ${MADS_TEST_SSL_LIBS}
  )
  target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(${test_name} PRIVATE
    MADS_TEST_FIXTURES_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures"
    MADS_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  catch_discover_tests(${test_name} PROPERTIES TIMEOUT 120)
endforeach()
