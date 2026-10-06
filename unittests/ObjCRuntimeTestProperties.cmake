# Apply after GoogleTest's PRE_TEST discovery. This case rebuilds and executes
# eight combinations of ownership, instrumentation and Mach-O fixups. Profiled
# Clang fixtures also contain compiler-runtime code; the complete matrix takes
# about five minutes on macOS arm64 even without concurrent CTest jobs.
list(FIND NeverDObjCRuntimeSourceTests_TESTS
  ObjCRuntimeSource.RecompiledBlocksPreserveEscapingCapturesAndOwnershipHelpers
  _neverd_block_matrix_index)
if(NOT _neverd_block_matrix_index EQUAL -1)
  set_tests_properties(
    ObjCRuntimeSource.RecompiledBlocksPreserveEscapingCapturesAndOwnershipHelpers
    PROPERTIES TIMEOUT 900 PROCESSORS 4)
endif()
unset(_neverd_block_matrix_index)
