find_program(LOWDOWN_EXECUTABLE lowdown)

macro(create_man target)
  add_custom_target(man_${target} ALL
      COMMAND lowdown -s -Tman -o ${target}.1 ${target}.md
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
  install(FILES ${target}.1 DESTINATION "share/man/man1")
endmacro()

if(LOWDOWN_EXECUTABLE)
  set(man_pages "mads" "broker")
  foreach(page ${man_pages})
    create_man(${page})
  endforeach()
else()
  message(WARNING "Command 'lowdown' not found: skipping generating man pages")
endif()