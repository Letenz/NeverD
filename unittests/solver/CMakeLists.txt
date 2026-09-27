add_neverd_unittest(NeverDSolverTests
  BitBlasterTests.cpp
  BitVectorSolverTests.cpp
  CnfEncoderTests.cpp
  SatSolverTests.cpp
  Z3SolverTests.cpp
  Z3DifferentialTests.cpp

  LINK_COMPONENTS Solver
)

target_include_directories(NeverDSolverTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

if(NEVERD_ENABLE_Z3)
  target_compile_definitions(NeverDSolverTests PRIVATE NEVERD_Z3_ORACLE=1)
  target_link_libraries(NeverDSolverTests PRIVATE NeverDZ3)
endif()
