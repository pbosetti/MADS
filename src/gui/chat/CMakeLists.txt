find_package(Threads REQUIRED)

mads_gui_executable(chat main.cpp ${imgui_color_text_edit_SOURCE_DIR}/TextEditor.cpp)
target_include_directories(${MADS_GUI_TARGET} PRIVATE ${imgui_color_text_edit_SOURCE_DIR})
target_link_libraries(${MADS_GUI_TARGET} PRIVATE
  mads_gui_common
  portable_file_dialogs
  MadsCore
  MadsMainDeps
)
if(UNIX AND NOT APPLE)
  target_link_libraries(${MADS_GUI_TARGET} PRIVATE ssl crypto Threads::Threads)
endif()
if(WIN32)
  target_link_libraries(${MADS_GUI_TARGET} PRIVATE comdlg32 ole32 shell32)
endif()
