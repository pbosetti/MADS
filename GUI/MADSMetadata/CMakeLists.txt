cmake_minimum_required(VERSION 3.5)

project(MadsMetadata VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        qjsonmodel.cpp
        qjsonmodel.h
        toggleswitch.cpp
        toggleswitch.h
        colorrepository.cpp
        colorrepository.h
)

include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/pbosetti/tomlplusplus.git
    GIT_TAG        050c7a0
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus nlohmann_json)

include_directories(
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/../../src
  ${PROJECT_SOURCE_DIR}/../../products/include
  ${tomlplusplus_SOURCE_DIR}/include
  ${nlohmann_json_SOURCE_DIR}/include
)

link_directories(
  ${PROJECT_SOURCE_DIR}/../../products/lib
)

# Disable some warnings in Visual Studio
if (MSVC)
  add_compile_options(/wd4005)
endif()

if(WIN32)
  file(GLOB MY_LIBS "${PROJECT_SOURCE_DIR}/../../products/lib/*.lib")
  set(LIB_LIST ${MY_LIBS}  Ws2_32 Iphlpapi)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DZMQ_STATIC")
  set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -DZMQ_STATIC")
else()
  set(LIB_LIST pthread zmqpp-static zmq snappy)
endif()

add_compile_definitions(
  MADS_AGENT_NO_INFO
  APP_DOMAIN="MADS"
  APP_NAME="Metadata"
)


if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(MadsMetadata
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        qagent.h qagent.cpp
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET MadsMetadata APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(MadsMetadata SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(MadsMetadata
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(MadsMetadata PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Network
  ${LIB_LIST}
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.MadsMetadata)
endif()
set_target_properties(MadsMetadata PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS MadsMetadata
    BUNDLE DESTINATION ${CMAKE_SOURCE_DIR}/products
    RUNTIME DESTINATION ${CMAKE_SOURCE_DIR}/products
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(MadsMetadata)
endif()
