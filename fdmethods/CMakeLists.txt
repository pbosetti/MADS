cmake_minimum_required(VERSION 3.20)

project(fdmethods VERSION 1.0.0 LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FDMETHODS_USE_DOUBLE "Build SlidingDFT with double-precision samples" OFF)
option(FDMETHODS_BUILD_TESTS "Build fdmethods tests" ${PROJECT_IS_TOP_LEVEL})
option(FDMETHODS_BUILD_PYTHON "Build the Python extension modules" ${PROJECT_IS_TOP_LEVEL})

# ============================================================
# SlidingDFT targets
# ============================================================

add_library(SlidingDFT STATIC
  src/sliding_dft.cpp
)

add_library(fdmethods::SlidingDFT ALIAS SlidingDFT)

add_library(SlidingDFTC SHARED
  src/c_api.cpp
)

add_library(fdmethods::SlidingDFTC ALIAS SlidingDFTC)

target_include_directories(SlidingDFT
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(SlidingDFTC
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(SlidingDFT
  PUBLIC
    $<$<BOOL:${FDMETHODS_USE_DOUBLE}>:SLIDINGDFT_USE_DOUBLE>
)

target_compile_definitions(SlidingDFTC
  PUBLIC
    $<$<BOOL:${FDMETHODS_USE_DOUBLE}>:SLIDINGDFT_USE_DOUBLE>
    SLIDINGDFT_C_EXPORTS
)

target_link_libraries(SlidingDFTC PRIVATE SlidingDFT)

# ============================================================
# Goertzel targets
# ============================================================

add_library(goertzel INTERFACE)
add_library(fdmethods::goertzel ALIAS goertzel)
target_compile_features(goertzel INTERFACE cxx_std_20)
target_include_directories(
  goertzel
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_library(goertzel_c STATIC src/goertzel_c.cpp)
add_library(fdmethods::goertzel_c ALIAS goertzel_c)
target_compile_features(goertzel_c PUBLIC cxx_std_20)
target_include_directories(
  goertzel_c
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(goertzel_c PUBLIC goertzel)

# ============================================================
# Example executable
# ============================================================

add_executable(sliding_dft_example
  src/example.cpp
)

target_link_libraries(sliding_dft_example PRIVATE SlidingDFT)

# ============================================================
# Tests
# ============================================================

if(FDMETHODS_BUILD_TESTS)
  add_test(
    NAME sliding_dft_example_runs
    COMMAND sliding_dft_example
  )

  add_executable(goertzel_dtmf_test tests/dtmf_test.cpp)
  target_compile_features(goertzel_dtmf_test PRIVATE cxx_std_20)
  target_link_libraries(goertzel_dtmf_test PRIVATE goertzel)
  add_test(NAME dtmf_key_6 COMMAND goertzel_dtmf_test)
endif()

# ============================================================
# Python extension modules
# ============================================================

if(FDMETHODS_BUILD_PYTHON)
  find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

  Python3_add_library(goertzel_python MODULE WITH_SOABI python/goertzel_module.cpp)
  target_compile_features(goertzel_python PRIVATE cxx_std_20)
  target_include_directories(goertzel_python PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(goertzel_python PRIVATE Python3::Module goertzel)
  set_target_properties(goertzel_python PROPERTIES OUTPUT_NAME goertzel)
endif()

# ============================================================
# Compiler options
# ============================================================

if(MSVC)
  target_compile_options(SlidingDFT PRIVATE /W4 /permissive-)
  target_compile_options(SlidingDFTC PRIVATE /W4 /permissive-)
  target_compile_options(sliding_dft_example PRIVATE /W4 /permissive-)
  target_compile_options(goertzel_c PRIVATE /W4 /permissive-)
else()
  target_compile_options(SlidingDFT PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(SlidingDFTC PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(sliding_dft_example PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(goertzel_c PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|amd64|AMD64|i[3-6]86)$")
  target_compile_options(SlidingDFT PRIVATE -mavx2)
  target_compile_options(SlidingDFTC PRIVATE -mavx2)
  target_compile_options(sliding_dft_example PRIVATE -mavx2)
endif()

set_target_properties(SlidingDFT PROPERTIES OUTPUT_NAME SlidingDFT)
set_target_properties(SlidingDFT PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(SlidingDFTC PROPERTIES OUTPUT_NAME SlidingDFTC)
set_target_properties(goertzel_c PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ============================================================
# Install
# ============================================================

install(
  TARGETS SlidingDFT SlidingDFTC goertzel goertzel_c
  EXPORT fdmethodsTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(TARGET goertzel_python)
  install(TARGETS goertzel_python LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(
  DIRECTORY include/SlidingDFT
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  DIRECTORY include/goertzel
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  EXPORT fdmethodsTargets
  FILE fdmethodsTargets.cmake
  NAMESPACE fdmethods::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fdmethods
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/fdmethodsConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fdmethodsConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/fdmethodsConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fdmethods
)

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/fdmethodsConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/fdmethodsConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fdmethods
)
