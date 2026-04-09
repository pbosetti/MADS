#   ____  _             _           
#  |  _ \| |_   _  __ _(_)_ __  ___ 
#  | |_) | | | | |/ _` | | '_ \/ __|
#  |  __/| | |_| | (_| | | | | \__ \
#  |_|   |_|\__,_|\__, |_|_| |_|___/
#                 |___/             
# Default plugins for Source, filter and sink agents

if(MSVC)

else()
  add_compile_options(-Wno-unused-parameter)
endif()

# Source plugin
add_library(publish SHARED ${SOURCE_DIR}/plugin/source_default.cpp)
set_target_properties(publish PROPERTIES PREFIX "" SUFFIX ".plugin")
target_link_libraries(publish PRIVATE pugg rang)
list(APPEND TARGET_LIST publish)

# Filter plugin
add_library(bridge SHARED ${SOURCE_DIR}/plugin/filter_default.cpp)
set_target_properties(bridge PROPERTIES PREFIX "" SUFFIX ".plugin")
target_link_libraries(bridge PRIVATE pugg rang)
list(APPEND TARGET_LIST bridge)

# Sink plugin
add_library(feedback SHARED ${SOURCE_DIR}/plugin/sink_default.cpp)
set_target_properties(feedback PROPERTIES PREFIX "" SUFFIX ".plugin")
target_link_libraries(feedback PRIVATE pugg rang)
list(APPEND TARGET_LIST feedback)

# Monitor plugin
add_library(monitor SHARED ${SOURCE_DIR}/plugin/monitor.cpp)
set_target_properties(monitor PROPERTIES PREFIX "" SUFFIX ".plugin")
target_link_libraries(monitor PRIVATE pugg rang)
list(APPEND TARGET_LIST monitor)

# replay plugin
if(MADS_ENABLE_MONGOCXX)
  add_library(replay SHARED ${SOURCE_DIR}/plugin/mongo_replay.cpp)
  if(APPLE)
    set(_mads_plugin_rel_rpath "@loader_path/../lib")
  elseif(UNIX)
    set(_mads_plugin_rel_rpath "\$ORIGIN/../lib")
  endif()
  if(DEFINED _mads_plugin_rel_rpath)
    set_target_properties(
      replay
      PROPERTIES
        PREFIX ""
        SUFFIX ".plugin"
        BUILD_RPATH "${_mads_plugin_rel_rpath}"
        INSTALL_RPATH "${_mads_plugin_rel_rpath}"
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )
  else()
    set_target_properties(replay PROPERTIES PREFIX "" SUFFIX ".plugin")
  endif()
  target_link_libraries(replay PRIVATE MadsCore pugg rang)
  link_first_available(replay CANDIDATES mongo::mongocxx_static mongocxx_static)
  link_first_available(replay CANDIDATES mongo::bsoncxx_static bsoncxx_static)
  list(APPEND TARGET_LIST replay)
endif()

# Install plugins
install(TARGETS ${TARGET_LIST}
  BUNDLE DESTINATION bin
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION lib
  COMPONENT MadsApps
)
