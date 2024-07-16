#   ____  _             _           
#  |  _ \| |_   _  __ _(_)_ __  ___ 
#  | |_) | | | | |/ _` | | '_ \/ __|
#  |  __/| | |_| | (_| | | | | \__ \
#  |_|   |_|\__,_|\__, |_|_| |_|___/
#                 |___/             
# Default plugins for Source, filter and sink agents

# Source plugin
add_library(publish SHARED ${SOURCE_DIR}/plugin/source_default.cpp)
set_target_properties(publish PROPERTIES PREFIX "" SUFFIX ".plugin")
target_link_libraries(publish pugg)
list(APPEND TARGET_LIST publish)

# Filter plugin
add_library(bridge SHARED ${SOURCE_DIR}/plugin/filter_default.cpp)
set_target_properties(bridge PROPERTIES PREFIX "" SUFFIX ".plugin")
target_link_libraries(bridge pugg)
list(APPEND TARGET_LIST bridge)

# Sink plugin
add_library(feedback SHARED ${SOURCE_DIR}/plugin/sink_default.cpp)
set_target_properties(feedback PROPERTIES PREFIX "" SUFFIX ".plugin")
target_link_libraries(feedback pugg)
list(APPEND TARGET_LIST feedback)

# Install plugins
install(TARGETS ${TARGET_LIST}
  BUNDLE DESTINATION bin
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION lib
)