# Apply after PRE_TEST discovery. This original ARM64 comparison retains its
# guest deadline while running without competing CTest workloads.
file(READ "${CMAKE_CURRENT_LIST_DIR}/WindowsTestExecution.def"
  _neverd_windows_execution_inventory)
string(REGEX MATCH
  "NEVERD_WINDOWS_TEST_EXECUTION_DIAGNOSTIC\\([ \t\r\n]*\"([^\"]+)\"[ \t\r\n]*\\)"
  _neverd_windows_execution_diagnostic
  "${_neverd_windows_execution_inventory}")
set(_neverd_windows_execution_error "${CMAKE_MATCH_1}")
string(CONCAT _neverd_windows_execution_pattern
  "NEVERD_WINDOWS_TEST_SERIAL\\([ \t\r\n]*\"([^\"]+)\","
  "[ \t\r\n]*\"([^\"]+)\",[ \t\r\n]*\"([^\"]+)\","
  "[ \t\r\n]*\"([^\"]+)\"[ \t\r\n]*\\)")
string(REGEX MATCH "${_neverd_windows_execution_pattern}"
  _neverd_windows_execution_rule "${_neverd_windows_execution_inventory}")
if(NOT _neverd_windows_execution_rule)
  message(FATAL_ERROR "${_neverd_windows_execution_error}")
endif()
set(_neverd_windows_execution_test
  "${CMAKE_MATCH_1}/${CMAKE_MATCH_2}.${CMAKE_MATCH_3}/${CMAKE_MATCH_4}")
if(_neverd_windows_execution_test IN_LIST NeverDWindowsProcessTests_TESTS)
  set_tests_properties("${_neverd_windows_execution_test}" PROPERTIES
    RUN_SERIAL TRUE)
endif()
unset(_neverd_windows_execution_inventory)
unset(_neverd_windows_execution_diagnostic)
unset(_neverd_windows_execution_error)
unset(_neverd_windows_execution_pattern)
unset(_neverd_windows_execution_rule)
unset(_neverd_windows_execution_test)
