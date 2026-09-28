add_neverd_component_library(NeverDInterpreterSpecialization
  InterpreterSpecialization.cpp
  InterpreterMachineState.cpp
  FiniteValues.cpp
  ControlDiscovery.cpp

  LINK_COMPONENTS
  IR
  Symbolic
  Solver
)
