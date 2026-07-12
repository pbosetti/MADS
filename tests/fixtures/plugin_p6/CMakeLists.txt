#   ____  _             _
#  |  _ \| |_   _  __ _(_)_ __
#  | |_) | | | | |/ _` | | '_ \
#  |  __/| | |_| | (_| | | | | |
#  |_|   |_|\__,_|\__, |_|_| |_|
#                 |___/
# Fake plugin project (fixture) used by tests/test_plugin_migrate.cpp to drive
# Mads::PluginMigrate::run() end to end. Modeled after share/templates/CMakeLists.txt
# but pinned at protocol P6, i.e. pre-P6->P7 migration.
cmake_minimum_required(VERSION 3.28)
project(p6demo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

include(FetchContent)
# pugg is for the plugin system
FetchContent_Declare(pugg
  GIT_REPOSITORY https://github.com/pbosetti/pugg.git
  GIT_TAG        1.0.0
  GIT_SHALLOW    TRUE
)

FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(pugg json)

# NOTE: keep this block's only GIT_TAG-with-protocol-suffix token being the
# real pin below; detect_protocol() is a plain (non-comment-aware) regex scan,
# so a second such token anywhere in the block -- even inside a "#" comment --
# would be picked up first. (bump_git_tag()'s comment-awareness is exercised
# with synthetic strings in test_plugin_migrate.cpp instead.)
FetchContent_Populate(plugin
  # pinned to the mads_plugin repo at the P6 protocol tag
  GIT_REPOSITORY https://github.com/pbosetti/mads_plugin.git
  GIT_TAG        v2.3-P6
  GIT_SHALLOW    TRUE
  SUBBUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/plugin-subbuild
  SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/plugin-src
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/plugin-build
)

include_directories(${plugin_SOURCE_DIR}/src)
include_directories(${json_SOURCE_DIR}/include)

add_library(p6demo SHARED ${SRC_DIR}/p6demo.cpp ${SRC_DIR}/p6demo_helper.cpp)
target_link_libraries(p6demo PRIVATE pugg)
set_target_properties(p6demo PROPERTIES PREFIX "" SUFFIX ".plugin")
