add_neverd_component_library(NeverDBytecodeAnalysis
  BytecodeDecoder.cpp
  BytecodeProfile.cpp
  BytecodeSource.cpp
  LINK_COMPONENTS IR
  LLVM_COMPONENTS Support
)
