mads_gui_executable(ini main.cpp)
target_link_libraries(${MADS_GUI_TARGET} PRIVATE mads_gui_common mads_imnodes)
