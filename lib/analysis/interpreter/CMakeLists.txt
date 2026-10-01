add_neverd_component_library(NeverDInterpreterSpecialization
  InterpreterSpecialization.cpp
  NativeStackControl.cpp
  InterpreterMachineState.cpp
  FiniteValues.cpp
  FiniteQueryCache.cpp
  FrameOffsets.cpp
  StringTransfer.cpp
  ControlDiscovery.cpp
  LowIRUndefinedIndependence.cpp
  LowIRLoopPlanPairing.cpp
  LowIRLoopAlignment.cpp

  LINK_COMPONENTS
  IR
  Symbolic
  Solver
)
