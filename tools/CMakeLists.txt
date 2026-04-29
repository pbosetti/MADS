#   _____           _     
#  |_   _|__   ___ | |___ 
#    | |/ _ \ / _ \| / __|
#    | | (_) | (_) | \__ \
#    |_|\___/ \___/|_|___/
                        


# Lists are local: clean it up before starting
unset(TARGET_LIST)

# TARGETS ######################################################################

# find_package(OpenCV)
# if(OpenCV_FOUND)
#   message(STATUS "OpenCV found")
#   include_directories( ${OpenCV_INCLUDE_DIRS} )
#   add_executable(img_browser ${CMAKE_CURRENT_LIST_DIR}/img_browser.cpp)
#   target_link_libraries(img_browser ${OpenCV_LIBS} ${MONGO_LIBS})
#   list(APPEND TARGET_LIST img_browser)
# else()
#   message(WARNING "OpenCV not found")
# endif()

add_executable(service_discovery_demo ${CMAKE_CURRENT_LIST_DIR}/service_discovery_demo.cpp)
target_include_directories(
  service_discovery_demo
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(
  service_discovery_demo
  PRIVATE
    MadsCore
    cxxopts
    nlohmann_json::nlohmann_json
)
list(APPEND TARGET_LIST service_discovery_demo)


# INSTALL ######################################################################
set(CMAKE_INSTALL_PREFIX ${USR_DIR})
install(TARGETS ${TARGET_LIST} DESTINATION bin)
