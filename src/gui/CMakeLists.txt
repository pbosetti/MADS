include(FetchContent)

option(MADS_GUI_ENABLE_WAYLAND "Enable the GLFW Wayland backend for the GUI apps (needs wayland-scanner and Wayland dev packages)" OFF)

# GLFW options must be set before the dependency is declared/made available
set(GLFW_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
  set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND ${MADS_GUI_ENABLE_WAYLAND} CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE
)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.6
  GIT_SHALLOW TRUE
)
FetchContent_Declare(portable_file_dialogs
  GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
  GIT_TAG 0.1.0
)
FetchContent_Declare(imgui_color_text_edit
  GIT_REPOSITORY https://github.com/goossens/ImGuiColorTextEdit.git
  GIT_TAG v1.92.9
)

FetchContent_MakeAvailable(glfw)

# Populate only (no add_subdirectory of their own CMakeLists)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_Populate(imgui)
endif()
FetchContent_GetProperties(portable_file_dialogs)
if(NOT portable_file_dialogs_POPULATED)
  FetchContent_Populate(portable_file_dialogs)
endif()
FetchContent_GetProperties(imgui_color_text_edit)
if(NOT imgui_color_text_edit_POPULATED)
  FetchContent_Populate(imgui_color_text_edit)
endif()

find_package(OpenGL REQUIRED)

add_library(mads_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(mads_imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(mads_imgui PUBLIC glfw OpenGL::GL)
target_compile_definitions(mads_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
if(APPLE)
  target_compile_definitions(mads_imgui PUBLIC GL_SILENCE_DEPRECATION)
endif()
if(WIN32)
  target_compile_definitions(mads_imgui PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Silence third-party warnings.
foreach(_mads_gui_third_party glfw mads_imgui)
  if(MSVC)
    # glfw sets its own /W3 (and so may we); appending /w on top of either makes
    # MSVC emit a D9025 command-line warning ("overriding '/Wn' with '/w'").
    # Strip the existing /Wn flag from each target before silencing it so the
    # warning level ends up at /w cleanly, with no command-line conflict.
    get_target_property(_mads_gui_opts ${_mads_gui_third_party} COMPILE_OPTIONS)
    if(_mads_gui_opts)
      list(FILTER _mads_gui_opts EXCLUDE REGEX "^/W[0-4]$|^/Wall$")
      set_target_properties(${_mads_gui_third_party} PROPERTIES COMPILE_OPTIONS "${_mads_gui_opts}")
    endif()
    unset(_mads_gui_opts)
    target_compile_options(${_mads_gui_third_party} PRIVATE /w)
  else()
    target_compile_options(${_mads_gui_third_party} PRIVATE -w)
  endif()
endforeach()
unset(_mads_gui_third_party)

add_library(portable_file_dialogs INTERFACE)
target_include_directories(portable_file_dialogs INTERFACE
  ${portable_file_dialogs_SOURCE_DIR}
)

if(NOT DEFINED MADS_EXECUTABLE_PREFIX)
  set(MADS_EXECUTABLE_PREFIX "mads")
endif()

# Shared look & feel of the GUI apps (style, fonts, logo texture, status footer).
add_library(mads_gui_common STATIC common/gui_style.cpp)
target_include_directories(mads_gui_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/common
  ${PROJECT_SOURCE_DIR}/src   # stb_image.h, shipped by MADS
)
target_link_libraries(mads_gui_common PUBLIC mads_imgui)
# Fallback for running the GUI apps from the build tree (load_logo in gui_style.cpp).
target_compile_definitions(mads_gui_common PRIVATE MADS_GUI_SOURCE_IMAGES_DIR="${PROJECT_SOURCE_DIR}/share/images")

# Creates an executable named like the other MADS apps (mads-<name>) and
# returns its target name in MADS_GUI_TARGET (PARENT_SCOPE).
function(mads_gui_executable name)
  if(MADS_EXECUTABLE_PREFIX STREQUAL name)
    set(_output_name ${MADS_EXECUTABLE_PREFIX})
  elseif(MADS_EXECUTABLE_PREFIX STREQUAL "")
    set(_output_name "${name}")
  else()
    set(_output_name "${MADS_EXECUTABLE_PREFIX}-${name}")
  endif()

  add_executable("${_output_name}" ${ARGN})
  if(APPLE)
    set(_mads_rel_rpath "@loader_path/../lib")
  elseif(UNIX)
    set(_mads_rel_rpath "\$ORIGIN/../lib")
  endif()
  if(DEFINED _mads_rel_rpath)
    set_target_properties("${_output_name}" PROPERTIES
      BUILD_RPATH "${_mads_rel_rpath}"
      INSTALL_RPATH "${_mads_rel_rpath}"
      INSTALL_RPATH_USE_LINK_PATH FALSE
    )
  endif()
  install(TARGETS "${_output_name}"
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT MadsGui
  )
  set(MADS_GUI_TARGET ${_output_name} PARENT_SCOPE)
endfunction()

add_subdirectory(director)
add_subdirectory(chat)
