add_library(mads_director_core STATIC
  attach_client.cpp
  attach_launcher.cpp
  example_config.cpp
  process_manager.cpp
  platform_process_posix.cpp
  platform_process_windows.cpp
)
target_include_directories(mads_director_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mads_director_core PUBLIC MadsCore)
target_compile_definitions(mads_director_core PUBLIC MADS_DIRECTOR_EXAMPLE_CONFIG)

if(NOT DEFINED MADS_EXECUTABLE_PREFIX)
  set(MADS_EXECUTABLE_PREFIX "mads")
endif()

mads_gui_executable(director main.cpp gui_app.cpp)
target_link_libraries(${MADS_GUI_TARGET} PRIVATE mads_director_core mads_gui_common portable_file_dialogs)
if(UNIX AND NOT APPLE)
  # MadsCore leaves its OpenSSL symbols (https_client) undefined on Linux: like
  # every executable in src/main, link ssl/crypto here.
  target_link_libraries(${MADS_GUI_TARGET} PRIVATE ssl crypto)
endif()
if(WIN32)
  target_link_libraries(${MADS_GUI_TARGET} PRIVATE comdlg32 ole32 shell32)
endif()
target_compile_definitions(${MADS_GUI_TARGET} PRIVATE
  DIRECTOR_CMD_PREFIX="${MADS_EXECUTABLE_PREFIX}"
)
