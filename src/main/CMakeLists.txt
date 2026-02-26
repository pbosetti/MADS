cmake_minimum_required(VERSION 3.24)


if(MSVC)
  add_compile_options(/wd4505) # Unused functions
  add_compile_options(/wd4005) # ZMQ_DEPRECATED
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()


macro(create_mads_target name main_file)
  set(options)
  set(one_value_args)
  set(multi_value_args SRCS LIBS DEFS)
  cmake_parse_arguments(MADS_TARGET "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT DEFINED MADS_EXECUTABLE_PREFIX)
    set(MADS_EXECUTABLE_PREFIX "mads")
  endif()

  if (MADS_EXECUTABLE_PREFIX STREQUAL ${name})
    set(_output_name ${MADS_EXECUTABLE_PREFIX})
  elseif(MADS_EXECUTABLE_PREFIX STREQUAL "")
    set(_output_name "${name}")
  else()
    set(_output_name "${MADS_EXECUTABLE_PREFIX}-${name}")
  endif()

  add_executable("${_output_name}" "${main_file}" ${MADS_TARGET_SRCS})
  if(APPLE)
    set(_mads_rel_rpath "@loader_path/../lib")
  elseif(UNIX)
    set(_mads_rel_rpath "\$ORIGIN/../lib")
  endif()
  if(DEFINED _mads_rel_rpath)
    set_target_properties(
      "${_output_name}"
      PROPERTIES
        BUILD_RPATH "${_mads_rel_rpath}"
        INSTALL_RPATH "${_mads_rel_rpath}"
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )
  endif()
  target_link_libraries("${_output_name}" PRIVATE MadsCore ${MADS_TARGET_LIBS})
  if(MADS_TARGET_DEFS)
    target_compile_definitions("${_output_name}" PRIVATE ${MADS_TARGET_DEFS})
  endif()
  if(TARGET MadsMainDeps)
    target_link_libraries("${_output_name}" PRIVATE MadsMainDeps)
  endif()
  list(APPEND MADS_MAIN_TARGETS "${_output_name}")
  set(MADS_MAIN_TARGETS "${MADS_MAIN_TARGETS}" PARENT_SCOPE)
endmacro()

file(GLOB MADS_MAIN_SOURCES "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
list(APPEND MADS_MAIN_EXCLUDE plugin_loader)
if(MSVC)
  list(APPEND MADS_MAIN_EXCLUDE image)
endif()

foreach(main_source IN LISTS MADS_MAIN_SOURCES)
  get_filename_component(main_name "${main_source}" NAME_WE)
  if(main_name IN_LIST MADS_MAIN_EXCLUDE)
    message(STATUS "Skipping executable ${main_name} (excluded)")
    continue()
  endif()
  message(STATUS "Preparing executable ${main_name} (from ${main_source})")
  create_mads_target("${main_name}" "${main_source}")
endforeach()

create_mads_target(source plugin_loader.cpp DEFS "PLUGIN_LOADER_SOURCE")
create_mads_target(filter plugin_loader.cpp DEFS "PLUGIN_LOADER_FILTER")
create_mads_target(sink plugin_loader.cpp DEFS "PLUGIN_LOADER_SINK")

if(MADS_MAIN_TARGETS)
  list(REMOVE_DUPLICATES MADS_MAIN_TARGETS)
  install(
    TARGETS ${MADS_MAIN_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT MadsApps
  )
endif()
