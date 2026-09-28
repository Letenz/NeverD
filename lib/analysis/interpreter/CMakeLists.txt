add_neverd_component_library(NeverDInterpreterSpecialization
  InterpreterSpecialization.cpp
  InterpreterMachineState.cpp
  FiniteValues.cpp
  FiniteQueryCache.cpp
  ControlDiscovery.cpp
  LowIRUndefinedIndependence.cpp

  LINK_COMPONENTS
  IR
  Symbolic
  Solver
)
