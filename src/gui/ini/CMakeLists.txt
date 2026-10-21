mads_gui_executable(ini main.cpp ini_app.cpp ui_state.cpp node_canvas.cpp inspector.cpp
  prefs.cpp remote_client.cpp ini_app_remote.cpp)
target_link_libraries(${MADS_GUI_TARGET} PRIVATE mads_gui_common mads_imnodes mads_ini_core portable_file_dialogs)
if(UNIX AND NOT APPLE)
  # MadsCore leaves its OpenSSL symbols (https_client) undefined on Linux: like
  # every executable in src/main, link ssl/crypto here.
  target_link_libraries(${MADS_GUI_TARGET} PRIVATE ssl crypto)
endif()
if(WIN32)
  target_link_libraries(${MADS_GUI_TARGET} PRIVATE comdlg32 ole32 shell32)
endif()
