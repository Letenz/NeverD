# Apply after PRE_TEST discovery. Batched cases run independently bounded
# workloads; their aggregate host deadline must cover the complete batch.
file(READ "${CMAKE_CURRENT_LIST_DIR}/AndroidTestExecution.def"
  _neverd_android_execution_inventory)
string(REGEX MATCH
  "NEVERD_ANDROID_TEST_EXECUTION_DIAGNOSTIC\\([ \t\r\n]*\"([^\"]+)\"[ \t\r\n]*\\)"
  _neverd_android_execution_diagnostic
  "${_neverd_android_execution_inventory}")
set(_neverd_android_execution_error "${CMAKE_MATCH_1}")
string(CONCAT _neverd_android_execution_pattern
  "NEVERD_ANDROID_TEST_EXECUTION\\([ \t\r\n]*\"([^\"]+)\","
  "[ \t\r\n]*\"([^\"]+)\",[ \t\r\n]*\"([^\"]+)\","
  "[ \t\r\n]*([1-9][0-9]*),[ \t\r\n]*([01])[ \t\r\n]*\\)")
string(REGEX MATCHALL "${_neverd_android_execution_pattern}"
  _neverd_android_execution_rules "${_neverd_android_execution_inventory}")
if(NOT _neverd_android_execution_rules)
  message(FATAL_ERROR "${_neverd_android_execution_error}")
endif()
foreach(_neverd_android_execution_rule IN LISTS _neverd_android_execution_rules)
  string(REGEX MATCH "${_neverd_android_execution_pattern}"
    _neverd_android_execution_match "${_neverd_android_execution_rule}")
  set(_neverd_android_execution_prefix
    "${CMAKE_MATCH_1}/${CMAKE_MATCH_2}.${CMAKE_MATCH_3}/")
  set(_neverd_android_execution_timeout "${CMAKE_MATCH_4}")
  set(_neverd_android_execution_serial "${CMAKE_MATCH_5}")
  foreach(_neverd_android_test IN LISTS NeverDAndroidNativeTests_TESTS)
    string(FIND "${_neverd_android_test}" "${_neverd_android_execution_prefix}"
      _neverd_android_execution_match)
    if(_neverd_android_execution_match EQUAL 0)
      set_tests_properties("${_neverd_android_test}" PROPERTIES
        TIMEOUT "${_neverd_android_execution_timeout}"
        RUN_SERIAL "${_neverd_android_execution_serial}")
    endif()
  endforeach()
endforeach()
unset(_neverd_android_test)
unset(_neverd_android_execution_inventory)
unset(_neverd_android_execution_diagnostic)
unset(_neverd_android_execution_error)
unset(_neverd_android_execution_pattern)
unset(_neverd_android_execution_rule)
unset(_neverd_android_execution_rules)
unset(_neverd_android_execution_prefix)
unset(_neverd_android_execution_timeout)
unset(_neverd_android_execution_serial)
unset(_neverd_android_execution_match)
