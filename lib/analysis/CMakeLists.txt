add_subdirectory(interpreter)

add_neverd_component_library(NeverDBinaryInterpreterSpecialization
  BinaryInterpreterSpecialization.cpp
  LINK_COMPONENTS InterpreterSpecialization Decode Loader
)

add_neverd_component_library(NeverDLLVMInterpreterModel
  llvm/LLVMInterpreterMachineState.cpp
  llvm/LLVMInterpreterModelContract.cpp
  llvm/LLVMInterpreterModelControl.cpp
  llvm/LLVMInterpreterModelMemory.cpp
  llvm/LLVMInterpreterModelScalar.cpp
  LINK_COMPONENTS InterpreterSpecialization
  LLVM_COMPONENTS Core Support
)

add_neverd_component_library(NeverDInterpreterLLVMRefinement
  llvm/InterpreterLLVMPreparation.cpp
  llvm/InterpreterLLVMRefinement.cpp
  LINK_COMPONENTS BinaryInterpreterSpecialization LLVMInterpreterModel
  LLVM_COMPONENTS AsmParser Core Support
)
