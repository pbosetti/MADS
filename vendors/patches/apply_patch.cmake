# Applies a git-format patch to the current directory (a FetchContent source
# dir), idempotently: a patch that is already applied is skipped, so a
# re-run of the patch step (e.g. after the patch file changed) does not fail.
#
#   cmake -DPATCH_FILE=<file.patch> -P apply_patch.cmake
if(NOT PATCH_FILE)
  message(FATAL_ERROR "apply_patch.cmake: PATCH_FILE not set")
endif()
find_package(Git REQUIRED QUIET)
get_filename_component(_name ${PATCH_FILE} NAME)

execute_process(
  COMMAND ${GIT_EXECUTABLE} apply --check --reverse ${PATCH_FILE}
  RESULT_VARIABLE _applied OUTPUT_QUIET ERROR_QUIET)
if(_applied EQUAL 0)
  message(STATUS "Patch ${_name}: already applied")
  return()
endif()

execute_process(
  COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${PATCH_FILE}
  RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "Patch ${_name} does not apply:\n${_err}")
endif()
message(STATUS "Patch ${_name}: applied")
