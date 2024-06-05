
find_package(Python3 COMPONENTS Interpreter Development)
find_package (PythonLibs REQUIRED)

message(STATUS "Python version: ${Python3_VERSION}")
message(STATUS "Python include dirs: ${Python3_INCLUDE_DIRS}") 
message(STATUS "Python libraries: ${Python3_LIBRARY_DIRS}")

if(MADS_ENABLE_SWIG)
  message(STATUS "SWIG is enabled")
  add_custom_target(
    python_wrapper ALL
    COMMAND swig -c++ -python -o agent.cpp agent.swg
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    BYPRODUCTS agent.cpp
    SOURCES agent.swg
  )
endif()

add_library(mads_py SHARED ${SOURCE_DIR}/wrap/agent.cpp)
target_include_directories(mads_py PUBLIC ${Python3_INCLUDE_DIRS})
target_link_directories(mads_py PUBLIC ${Python3_LIBRARY_DIRS})
set_target_properties (mads_py PROPERTIES PREFIX "")
set_target_properties (mads_py PROPERTIES OUTPUT_NAME "_Mads")
if (WIN32)
  set_target_properties (mads_py PROPERTIES SUFFIX ".pyd")
else()
  set_target_properties (mads_py PROPERTIES SUFFIX ".so")
endif()
target_link_libraries(mads_py ${LIB_LIST} ${Python3_LIBRARIES})

set(CMAKE_INSTALL_PREFIX ${USR_DIR})
install(TARGETS mads_py DESTINATION bin)
install(PROGRAMS ${SOURCE_DIR}/wrap/Mads.py TYPE BIN)