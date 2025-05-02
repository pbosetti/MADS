cmake_minimum_required(VERSION 3.20)

#   ____       _   _   _                 
#  / ___|  ___| |_| |_(_)_ __   __ _ ___ 
#  \___ \ / _ \ __| __| | '_ \ / _` / __|
#   ___) |  __/ |_| |_| | | | | (_| \__ \
#  |____/ \___|\__|\__|_|_| |_|\__, |___/
#                              |___/     
set(LIB_NAME mads)
set(CPACK_PACKAGE_VERSION_MAJOR 1)
set(CPACK_PACKAGE_VERSION_MINOR 2)
set(CPACK_PACKAGE_VERSION_PATCH 4)
set(LIB_VERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
project(${LIB_NAME} VERSION ${LIB_VERSION} LANGUAGES CXX)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefix for executable ames
set(PREFIX "mads-")

#    ___        _   _                 
#   / _ \ _ __ | |_(_) ___  _ __  ___ 
#  | | | | '_ \| __| |/ _ \| '_ \/ __|
#  | |_| | |_) | |_| | (_) | | | \__ \
#   \___/| .__/ \__|_|\___/|_| |_|___/
#        |_|                          

option(MADS_ENABLE_LOGGER "Enable logger agent (connects to MongoDB)" ON)
if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  option(MADS_ENABLE_METADATA "Enable metadata agent" OFF)
  option(MADS_USE_GPIOD_2 "Use libgpiod version 2" ON)
else()
  set(MADS_ENABLE_METADATA OFF)
endif()
option(MADS_ENABLE_PERF_ASSESS "Enable performance assessment tool" OFF)
option(MADS_ENABLE_MONGODB_AWS 
       "Enable Cloud MongoDB (requires crypto library on linux)" OFF)
option(MADS_ENABLE_TOOLS "Compile tools (requires OpenCV)" OFF)
option(MADS_ENABLE_PYTHON_WRAPPER "Compile Python3 wrapper to Agent class" OFF)
option(MADS_ENABLE_GUI "Compile GUI (requires QT6)" OFF)
option(MADS_SKIP_EXTERNALS 
       "Skip external targets (assume already compiled)" OFF)
option(MADS_MINIMAL "Only compile filter and source executables" OFF)


#  __     __             
#  \ \   / /_ _ _ __ ___ 
#   \ \ / / _` | '__/ __|
#    \ V / (_| | |  \__ \
#     \_/ \__,_|_|  |___/
#

set(EXTERNAL_DIR ${CMAKE_CURRENT_LIST_DIR}/external)
set(USR_DIR ${CMAKE_CURRENT_LIST_DIR}/products)
set(SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
set(MAIN_DIR ${SOURCE_DIR}/main)

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
  )
endforeach()


# RPATH management
# This embeds the relative path of the dynamic library into the executable
# So that it can find the library across installations
# If this is not given, the executable only searches the library in its same 
# folder or in a system path (/usr/lib, /usr/local/lib)
if (APPLE)
  set(CMAKE_INSTALL_RPATH "@executable_path/../lib")
else()
  set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib;/usr/local/lib")
endif()

# Disable some warnings in Visual Studio
if (MSVC)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS)
  add_compile_options(/wd4068 /wd4005 /wd4267 /wd4244 /wd4804 /EHsc /MP)
else()
  add_compile_options("-Wno-pragmas")
endif()


#   ____                            _                 _           
#  |  _ \  ___ _ __   ___ _ __   __| | ___ _ __   ___(_) ___  ___ 
#  | | | |/ _ \ '_ \ / _ \ '_ \ / _` |/ _ \ '_ \ / __| |/ _ \/ __|
#  | |_| |  __/ |_) |  __/ | | | (_| |  __/ | | | (__| |  __/\__ \
#  |____/ \___| .__/ \___|_| |_|\__,_|\___|_| |_|\___|_|\___||___/
#             |_|                                                 

# Fetched targets
include(FetchContent)
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/pbosetti/tomlplusplus.git
    GIT_TAG        050c7a0
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(cxxopts
    GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
    GIT_TAG        v3.1.1
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(rang
    GIT_REPOSITORY https://github.com/agauniyal/rang.git
    GIT_TAG        v3.2
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(pugg 
  GIT_REPOSITORY https://github.com/pbosetti/pugg.git
  GIT_TAG        1.0.3
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(tomlplusplus nlohmann_json cxxopts rang pugg)

FetchContent_Populate(plugin 
  GIT_REPOSITORY https://github.com/pbosetti/mads_plugin.git
  GIT_TAG        HEAD
  GIT_SHALLOW    TRUE
  SUBBUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/plugin-subbuild
  SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/plugin-src
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/plugin-build
)

FetchContent_Populate(inja
  GIT_REPOSITORY https://github.com/pantor/inja.git
  GIT_TAG        v3.4.0
  GIT_SHALLOW    TRUE
  SUBBUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/inja-subbuild
  SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/inja-src
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/inja-build
)


#   ____        _ _     _            _   _   _                 
#  | __ ) _   _(_) | __| |  ___  ___| |_| |_(_)_ __   __ _ ___ 
#  |  _ \| | | | | |/ _` | / __|/ _ \ __| __| | '_ \ / _` / __|
#  | |_) | |_| | | | (_| | \__ \  __/ |_| |_| | | | | (_| \__ \
#  |____/ \__,_|_|_|\__,_| |___/\___|\__|\__|_|_| |_|\__, |___/
#                                                    |___/     

include_directories(
  ${USR_DIR}/include 
  ${USR_DIR}/include/mongocxx/v_noabi 
  ${USR_DIR}/include/bsoncxx/v_noabi
  ${tomlplusplus_SOURCE_DIR}/include
  ${nlohmann_json_SOURCE_DIR}/include
  ${cxxopts_SOURCE_DIR}/include
  ${rang_SOURCE_DIR}/include
  ${date_SOURCE_DIR}/include
  ${plugin_SOURCE_DIR}/src
  ${inja_SOURCE_DIR}/include
)
link_directories(${USR_DIR}/lib)

if(WIN32)
  file(GLOB LIB_ZMQ "${USR_DIR}/lib/libzmq*.lib")
  file(GLOB LIB_MONGOCXX "${USR_DIR}/lib/mongocxx-v_noabi*.lib")
  file(GLOB LIB_BSONCXX "${USR_DIR}/lib/bsoncxx-v_noabi*.lib")
  set(LIB_LIST zmqpp-static ${LIB_ZMQ} snappy Ws2_32 Iphlpapi)
  set(MONGO_LIBS 
    ${LIB_MONGOCXX} mongoc-static-1.0 
    ${LIB_BSONCXX} bson-static-1.0 
    Ws2_32 bcrypt Secur32 Crypt32 Dnsapi
  )
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DZMQ_STATIC")
  set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -DZMQ_STATIC")
  add_compile_options(/MP)
else()
  set(LIB_LIST pthread zmqpp-static zmq snappy)
  set(DLOPEN_LIB dl)
endif()

if(APPLE)
  include_directories("/opt/homebrew/include")
  link_directories("/opt/homebrew/lib/")
  find_library(SECURITY_LIBRARY Security)
  find_library(COREFOUNDATION_LIBRARY CoreFoundation)
  set(MONGO_LIBS mongocxx-static bsoncxx-static mongoc-static-1.0 bson-static-1.0 resolv sasl2 ${SECURITY_LIBRARY} ${COREFOUNDATION_LIBRARY} zstd)
elseif(UNIX AND NOT APPLE)
  find_package(OpenSSL COMPONENTS Crypto SSL REQUIRED)
  set(MONGO_LIBS mongocxx-static bsoncxx-static mongoc-static-1.0 bson-static-1.0 resolv)
  if(NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64")
    # Unexpectedly, these three libraries are not needed on RaspbianOS or aarch64
    set(MONGO_LIBS ${MONGO_LIBS} ssl crypto)
  endif()
endif()

# Macro for building exec targets
macro(create_exec target)
  set(multiValueArgs SRCS LIBS DEFS)
  cmake_parse_arguments(ARG "" "" "${multiValueArgs}" ${ARGN})
  add_executable(${PREFIX}${target} ${MAIN_DIR}/${ARG_SRCS})
  list(APPEND TARGET_LIST ${PREFIX}${target})
  target_link_libraries(${PREFIX}${target} ${LIB_LIST} ${ARG_LIBS})
  target_compile_definitions(${PREFIX}${target} PRIVATE ${ARG_DEFS})
endmacro()


#   _____                    _       
#  |_   _|_ _ _ __ __ _  ___| |_ ___ 
#    | |/ _` | '__/ _` |/ _ \ __/ __|
#    | | (_| | | | (_| |  __/ |_\__ \
#    |_|\__,_|_|  \__, |\___|\__|___/
#                 |___/              


# The following also work on WINDOWS!
if(NOT MADS_MINIMAL)
  create_exec(broker SRCS broker.cpp)
  create_exec(feedback SRCS feedback.cpp)
  create_exec(bridge SRCS bridge.cpp)
  # Not supported on Windows
  if(NOT WIN32)
    create_exec(image SRCS image.cpp)
  endif()
endif()

# Main command
add_executable(mads ${MAIN_DIR}/mads.cpp)
if(UNIX AND NOT APPLE)
  target_link_libraries(mads OpenSSL::Crypto OpenSSL::SSL)
else()
  target_link_libraries(mads)
endif()
list(APPEND TARGET_LIST mads)

create_exec(filter SRCS plugin_loader.cpp LIBS pugg ${DLOPEN_LIB} DEFS "PLUGIN_LOADER_FILTER")
create_exec(source SRCS plugin_loader.cpp LIBS  pugg ${DLOPEN_LIB} DEFS "PLUGIN_LOADER_SOURCE")
create_exec(sink SRCS plugin_loader.cpp LIBS  pugg ${DLOPEN_LIB} DEFS "PLUGIN_LOADER_SINK")
create_exec(dealer SRCS dealer.cpp)
create_exec(worker SRCS worker.cpp LIBS pugg)

# Conditionally built targets

if(${MADS_ENABLE_PERF_ASSESS})
  create_exec(perf_assess SRCS perf_assess.cpp)
endif()

if (${MADS_ENABLE_METADATA})
  if(${MADS_USE_GPIOD_2})
    create_exec(metadata SRCS metadata.cpp LIBS gpiodcxx gpiod DEFS "HAVE_GPIOD" "MADS_USE_GPIOD_2")
  else()
    create_exec(metadata SRCS metadata.cpp LIBS gpiod DEFS "HAVE_GPIOD")
  endif()
endif()

if (${MADS_ENABLE_LOGGER})
  create_exec(logger SRCS logger.cpp LIBS ${MONGO_LIBS})
endif()
  

if(${MADS_ENABLE_TOOLS})
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/tools)
endif()

if(${MADS_ENABLE_PYTHON_WRAPPER})
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/wrap)
endif()

if(${MADS_ENABLE_GUI})
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/GUI/MADSMetadata)
endif()

# Default plugins
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/plugin)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/share/man)

create_exec(plugin SRCS make_plugin.cpp)

#   _____      _                        _     
#  | ____|_  _| |_ ___ _ __ _ __   __ _| |___ 
#  |  _| \ \/ / __/ _ \ '__| '_ \ / _` | / __|
#  | |___ >  <| ||  __/ |  | | | | (_| | \__ \
#  |_____/_/\_\\__\___|_|  |_| |_|\__,_|_|___/
#

if(NOT ${MADS_SKIP_EXTERNALS})
  message(STATUS "Compiling third party libraries")
  include(external.cmake)  
  foreach(t IN ITEMS ${TARGET_LIST})
    add_dependencies(${t} libzmq)
    add_dependencies(${t} zmqpp)
    add_dependencies(${t} snappy)
  endforeach()
  if (${MADS_ENABLE_LOGGER})
    add_dependencies(${PREFIX}logger mongocxx)
  endif()
endif()


#   ___           _        _ _ 
#  |_ _|_ __  ___| |_ __ _| | |
#   | || '_ \/ __| __/ _` | | |
#   | || | | \__ \ || (_| | | |
#  |___|_| |_|___/\__\__,_|_|_|
#

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set_property(CACHE CMAKE_INSTALL_PREFIX PROPERTY VALUE ${USR_DIR})
endif()
install(TARGETS ${TARGET_LIST}
  BUNDLE DESTINATION bin
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION lib
  COMPONENT MADS
)
# install(FILES ${CMAKE_CURRENT_LIST_DIR}/mads.ini
#   TYPE SYSCONF
# )
install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/share/templates
  DESTINATION share
  COMPONENT MADS
)
if(NOT WIN32)
  set(SCRIPTS_FILES 
    ${CMAKE_CURRENT_LIST_DIR}/scripts/mads-logging
    ${CMAKE_CURRENT_LIST_DIR}/scripts/mads-command
  )
  install(PROGRAMS ${SCRIPTS_FILES}
    TYPE BIN
  )
endif()


#   _____           _               _             
#  |  __ \         | |             (_)            
#  | |__) |_ _  ___| | ____ _  __ _ _ _ __   __ _ 
#  |  ___/ _` |/ __| |/ / _` |/ _` | | '_ \ / _` |
#  | |  | (_| | (__|   < (_| | (_| | | | | | (_| |
#  |_|   \__,_|\___|_|\_\__,_|\__, |_|_| |_|\__, |
#                              __/ |         __/ |
#                             |___/         |___/ 
if(${MADS_SKIP_EXTERNALS})
  set(CPACK_PACKAGE_NAME "${LIB_NAME}")
  set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${LIB_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
  set(CPACK_PACKAGE_VENDOR "MADS Consortium")
  set(CPACK_PACKAGE_DIRECTORY ${CMAKE_SOURCE_DIR}/packages)
  set(CPACK_STRIP_FILES TRUE)
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
  set(CPACK_PACKAGE_CONTACT "paolo.bosetti@unitn.it")
  set(CMAKE_PROJECT_HOMEPAGE_URL "https://github.com/pbosetti/MADS")
  set(CPACK_PACKAGE_DESCRIPTION "MADS installer: Multi-Agent Distributed System")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_LIST_DIR}/LICENSE")
  if(APPLE)
    set(CPACK_GENERATOR "STGZ")
    if(CMAKE_OSX_ARCHITECTURES MATCHES ";")
      set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${LIB_VERSION}-${CMAKE_SYSTEM_NAME}-universal")
    endif()
  elseif(MSVC)
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
  else()
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libzstd1, libssl3, openssl")
    set(CPACK_DEBIAN_PACKAGE_NAME MADS)
    set(CPACK_GENERATOR "DEB")
  endif()
  include(CPack)
  message(STATUS "Packaging enabled")
else()
  message(STATUS "Packaging not available: to enable it, set MADS_SKIP_EXTERNALS to ON")
endif()

#   ____                                        _        _   _             
#  |  _ \  ___   ___ _   _ _ __ ___   ___ _ __ | |_ __ _| |_(_) ___  _ __  
#  | | | |/ _ \ / __| | | | '_ ` _ \ / _ \ '_ \| __/ _` | __| |/ _ \| '_ \ 
#  | |_| | (_) | (__| |_| | | | | | |  __/ | | | || (_| | |_| | (_) | | | |
#  |____/ \___/ \___|\__,_|_| |_| |_|\___|_| |_|\__\__,_|\__|_|\___/|_| |_|
                                                                         
include(FindDoxygen)
option(MADS_ENABLE_DOXYGEN "Enable doxygen documentation" OFF)
find_package(Doxygen)
if(DOXYGEN_FOUND AND MADS_ENABLE_DOXYGEN)
  file(GLOB MARKDOWN_FILES "${CMAKE_CURRENT_LIST_DIR}/*.md")
  set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/doc)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE ${CMAKE_CURRENT_LIST_DIR}/README.md)
  doxygen_add_docs(Doxygen
    ${SOURCE_DIR} ${MARKDOWN_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/doc
    CONFIG_FILE ${CMAKE_CURRENT_LIST_DIR}/doc/Doxyfile
  )
endif()
