# These native/C differential cases repeatedly invoke external compilers.
# Keep the normal per-case deadline and full matrix, without competing CTest
# compilation workloads. Apply after PRE_TEST GoogleTest discovery.
foreach(_neverd_fp_state_test IN LISTS NeverDX86FPStateAccuracyTests_TESTS)
  if(_neverd_fp_state_test MATCHES "^X86FPStateAccuracy\\.")
    set_tests_properties("${_neverd_fp_state_test}" PROPERTIES RUN_SERIAL TRUE)
  endif()
endforeach()
unset(_neverd_fp_state_test)
