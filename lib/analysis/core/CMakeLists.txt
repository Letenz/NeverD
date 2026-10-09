# Shared orchestration invokes explicit native adapters. Adapter selection is
# determined by the source contract, never by the architecture of the build host.
add_neverd_component_library(NeverDInterpreterSpecialization
  InterpreterSpecialization.cpp
  ../arch/x86_64/NativeStackControl.cpp
  ../arch/x86_64/X64Recovery.cpp
  ../arch/x86_64/InterpreterMachineState.cpp
  FiniteValues.cpp
  CompleteModel.cpp
  CompletedTargetFacts.cpp
  ConditionalImplication.cpp
  DomainCoverage.cpp
  FiniteQueryCache.cpp
  FrameOffsets.cpp
  ../arch/x86_64/StringTransfer.cpp
  ControlDiscovery.cpp
  LowIRUndefinedIndependence.cpp
  LowIRLoopPlanPairing.cpp
  LowIRLoopAlignment.cpp

  LINK_COMPONENTS
  IR
  Symbolic
  Solver
)
