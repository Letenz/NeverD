add_neverd_component_library(NeverDInterpreterSpecialization
  InterpreterSpecialization.cpp
  FiniteValues.cpp

  LINK_COMPONENTS
  IR
  Symbolic
  Solver
)
