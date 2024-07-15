#   ____  _             _           
#  |  _ \| |_   _  __ _(_)_ __  ___ 
#  | |_) | | | | |/ _` | | '_ \/ __|
#  |  __/| | |_| | (_| | | | | \__ \
#  |_|   |_|\__,_|\__, |_|_| |_|___/
#                 |___/             
# Default plugins for Source, filter and sink agents

# Sink plugin
add_library(feedback SHARED ${SOURCE_DIR}/plugin/sink_default.cpp)
set_target_properties(feedback PROPERTIES PREFIX "" SUFFIX ".plugin")
target_link_libraries(feedback pugg)
list(APPEND TARGET_LIST feedback)

# Source plugin
add_library(publish SHARED ${SOURCE_DIR}/plugin/source_default.cpp)
set_target_properties(publish PROPERTIES PREFIX "" SUFFIX ".plugin")
target_link_libraries(publish pugg)
list(APPEND TARGET_LIST publish)

# Install plugins
install(TARGETS ${TARGET_LIST}
  BUNDLE DESTINATION bin
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION lib
)