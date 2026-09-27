include(${CMAKE_SOURCE_DIR}/cmake/NeverDZ3.cmake)

add_neverd_component_library(NeverDSolver
  bv/BitBlastArith.cpp
  bv/BitBlastShift.cpp
  bv/BitBlaster.cpp
  bv/BitVectorSolver.cpp
  bv/SymSynthVerifier.cpp
  bv/Z3Solver.cpp
  cnf/CnfEncoder.cpp
  sat/SatAnalyze.cpp
  sat/SatHeuristic.cpp
  sat/SatPropagate.cpp
  sat/SatSolver.cpp

  LINK_COMPONENTS
  Symbolic
)

if(NEVERD_ENABLE_Z3)
  target_compile_definitions(NeverDSolver PRIVATE NEVERD_HAS_Z3=1)
  target_link_libraries(NeverDSolver PRIVATE NeverDZ3)
endif()
