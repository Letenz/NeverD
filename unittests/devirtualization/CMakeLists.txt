add_neverd_unittest(NeverDInterpreterSpecializationTests
  InterpreterSpecializationTests.cpp
  LINK_COMPONENTS InterpreterSpecialization
)

add_neverd_unittest(NeverDDevirtualizationSourceTests
  DevirtualizationSourceTests.cpp
  LINK_COMPONENTS Pipeline BackendC Support
)
target_compile_definitions(NeverDDevirtualizationSourceTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
add_dependencies(NeverDDevirtualizationSourceTests neverd)
