# Removes accumulated gcov runtime data (.gcda) under a directory.
#
# .gcda files hold execution counters keyed to the exact arc layout recorded in
# the matching .gcno at compile time. Recompiling a translation unit invalidates
# them, and the runtime then refuses to merge:
#
#   profiling: .../foo.cpp.gcda: cannot merge previous GCDA file: corrupt arc tag
#
# emitted once per mismatched arc, which for a restructured file means tens of
# thousands of lines. Stale counters also silently pollute the coverage report.
#
# Invoke with: cmake -D MADS_COVERAGE_DIR=<dir> -P ResetCoverageData.cmake
if(NOT DEFINED MADS_COVERAGE_DIR)
  message(FATAL_ERROR "MADS_COVERAGE_DIR must be set")
endif()

file(GLOB_RECURSE _mads_gcda "${MADS_COVERAGE_DIR}/*.gcda")
if(_mads_gcda)
  file(REMOVE ${_mads_gcda})
endif()
