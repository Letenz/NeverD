add_neverd_unittest(NeverDSymbolicTests
  SymConcreteTests.cpp
  SymDispatchTests.cpp
  SymExecTests.cpp
  SymExploreCLIContractTests.cpp
  SymExploreTests.cpp
  SymExprTests.cpp
  SymExprCompareTests.cpp
  SymExprMaskTests.cpp
  SymMBAArithmeticTests.cpp
  SymMBAComplementArithmeticTests.cpp
  SymMBAOffsetTests.cpp
  SymMBAOutcomeTests.cpp
  SymMBAParityTests.cpp
  SymMBAPolynomialTests.cpp
  SymMBARestoredTests.cpp
  SymMBASelectorTests.cpp
  SymMBAStressTests.cpp
  SymMBAWeightedTests.cpp
  SymMBAComplementTests.cpp
  SymMBATests.cpp
  SymParseTests.cpp
  SymSynthTests.cpp
  SymX87Tests.cpp

  LINK_COMPONENTS Symbolic
)

target_include_directories(NeverDSymbolicTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(NeverDSymbolicTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  SYMBOLIC_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_dependencies(NeverDSymbolicTests neverd)
