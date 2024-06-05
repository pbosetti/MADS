#   _____           _     
#  |_   _|__   ___ | |___ 
#    | |/ _ \ / _ \| / __|
#    | | (_) | (_) | \__ \
#    |_|\___/ \___/|_|___/
                        


# Lists are local: clean it up before starting
unset(TARGET_LIST)

# TARGETS ######################################################################

find_package(OpenCV)
if(OpenCV_FOUND)
  message(STATUS "OpenCV found")
  include_directories( ${OpenCV_INCLUDE_DIRS} )
  add_executable(img_browser ${CMAKE_CURRENT_LIST_DIR}/img_browser.cpp)
  target_link_libraries(img_browser ${OpenCV_LIBS} ${MONGO_LIBS})
  list(APPEND TARGET_LIST img_browser)
else()
  message(WARNING "OpenCV not found")
endif()


# INSTALL ######################################################################
set(CMAKE_INSTALL_PREFIX ${USR_DIR})
install(TARGETS ${TARGET_LIST} DESTINATION bin)