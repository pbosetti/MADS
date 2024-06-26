cmake_minimum_required(VERSION 3.20)

#   ____       _   _   _                 
#  / ___|  ___| |_| |_(_)_ __   __ _ ___ 
#  \___ \ / _ \ __| __| | '_ \ / _` / __|
#   ___) |  __/ |_| |_| | | | | (_| \__ \
#  |____/ \___|\__|\__|_|_| |_|\__, |___/
#                              |___/     
set(LIB_NAME mads)
set(LIB_VERSION 0.8.0)
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
  add_compile_options(/wd4068 /wd4005 /wd4267 /wd4244 /wd4804 /EHsc)
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
  GIT_TAG        1.0.2
  GIT_SHALLOW    TRUE
)

FetchContent_Declare(plugin 
  GIT_REPOSITORY https://github.com/pbosetti/mads_plugin.git
  GIT_TAG        HEAD
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(tomlplusplus nlohmann_json cxxopts rang pugg)
FetchContent_Populate(plugin)


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
)
link_directories(${USR_DIR}/lib)

if(WIN32)
  file(GLOB LIB_ZMQ "${USR_DIR}/lib/libzmq*.lib")
  set(LIB_LIST zmqpp-static ${LIB_ZMQ} snappy Ws2_32 Iphlpapi)
  set(MONGO_LIBS 
    mongocxx mongoc-static-1.0 
    bsoncxx bson-static-1.0 
    Ws2_32 bcrypt Secur32 Crypt32 Dnsapi
  )
  set(ARMA_LIBS 
    armadillo 
    ${EXTERNAL_DIR}/armadillo/examples/lib_win64/libopenblas.lib
  )
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DZMQ_STATIC")
  set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -DZMQ_STATIC")
else()
  set(LIB_LIST pthread zmqpp-static zmq snappy)
  set(ARMA_LIBS armadillo)
  set(DLOPEN_LIB dl)
endif()

if(APPLE)
  include_directories("/opt/homebrew/include")
  link_directories("/opt/homebrew/lib/")
  set(MONGO_LIBS mongocxx bsoncxx)
else()
  set(MONGO_LIBS mongocxx-static bsoncxx-static mongoc-static-1.0 bson-static-1.0 resolv)
endif()

# Macro for building exec targets
macro(create_exec target )
  set(libs ${ARGV})
  list(REMOVE_ITEM libs ${target})
  add_executable(${PREFIX}${target} ${MAIN_DIR}/${target}.cpp)
  target_link_libraries(${PREFIX}${target} ${LIB_LIST} ${libs})
  list(APPEND TARGET_LIST ${PREFIX}${target})
endmacro()


#   _____                    _       
#  |_   _|_ _ _ __ __ _  ___| |_ ___ 
#    | |/ _` | '__/ _` |/ _ \ __/ __|
#    | | (_| | | | (_| |  __/ |_\__ \
#    |_|\__,_|_|  \__, |\___|\__|___/
#                 |___/              


# The following also work on WINDOWS!
if(NOT MADS_MINIMAL)
  create_exec(broker)
  create_exec(feedback)
  create_exec(bridge)
  # Not supported on Windows
  if(NOT WIN32)
  create_exec(image)
  endif()
endif()
create_exec(filter pugg ${DLOPEN_LIB})
create_exec(source pugg ${DLOPEN_LIB})
create_exec(sink pugg ${DLOPEN_LIB})

# Conditionally built targets

if(${MADS_ENABLE_PERF_ASSESS})
  create_exec(perf_assess)
endif()

if (${MADS_ENABLE_METADATA})
  add_compile_definitions("HAVE_GPIOD")
  if(${MADS_USE_GPIOD_2})
    add_compile_definitions("MADS_USE_GPIOD_2")
    set(GPIOD_LIB gpiodcxx gpiod)
  else()
    set(GPIOD_LIB gpiod)
  endif()
  create_exec(metadata ${GPIOD_LIB})
endif()

if (${MADS_ENABLE_LOGGER})
  create_exec(logger ${MONGO_LIBS})
endif()
  

if(${MADS_ENABLE_TOOLS})
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/tools)
endif()

if(${MADS_ENABLE_PYTHON_WRAPPER})
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/wrap)
endif()


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
)
install(FILES ${CMAKE_CURRENT_LIST_DIR}/mads.ini
  TYPE SYSCONF
)
file(GLOB SCRIPTS_FILES "${CMAKE_CURRENT_LIST_DIR}/scripts/*")
install(PROGRAMS ${SCRIPTS_FILES}
  TYPE BIN
)
# Install custom built OpenBLAS provided by armadillo
if(WIN32)
  install(FILES ${EXTERNAL_DIR}/armadillo/examples/lib_win64/libopenblas.dll 
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

set(CPACK_PACKAGE_NAME ${LIB_NAME})
set(CPACK_PACKAGE_VENDOR "MADS Consortium")
set(CPACK_GENERATOR "TGZ")
set(CPACK_INSTALLED_DIRECTORIES 
  ${CMAKE_INSTALL_PREFIX}/lib ./lib
  ${CMAKE_INSTALL_PREFIX}/include ./include
)
set(CPACK_PACKAGE_VERSION ${LIB_VERSION})
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_SOURCE_DIR}/packages)
set(CPACK_STRIP_FILES TRUE)
include(CPack)


#   ____                                        _        _   _             
#  |  _ \  ___   ___ _   _ _ __ ___   ___ _ __ | |_ __ _| |_(_) ___  _ __  
#  | | | |/ _ \ / __| | | | '_ ` _ \ / _ \ '_ \| __/ _` | __| |/ _ \| '_ \ 
#  | |_| | (_) | (__| |_| | | | | | |  __/ | | | || (_| | |_| | (_) | | | |
#  |____/ \___/ \___|\__,_|_| |_| |_|\___|_| |_|\__\__,_|\__|_|\___/|_| |_|
                                                                         
include(FindDoxygen)
find_package(Doxygen)
if(DOXYGEN_FOUND)
  file(GLOB MARKDOWN_FILES "${CMAKE_CURRENT_LIST_DIR}/*.md")
  option(MADS_ENABLE_DOXYGEN "Enable doxygen documentation" OFF)
  set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/doc)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE ${CMAKE_CURRENT_LIST_DIR}/README.md)
  doxygen_add_docs(Doxygen
    ${SOURCE_DIR} ${MARKDOWN_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/doc
    CONFIG_FILE Doxyfile
  )
endif()
