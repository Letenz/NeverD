add_neverd_component_library(NeverDEmulationDarwinKernel
  DarwinServices.cpp
  DarwinMemory.cpp
  DarwinFiles.cpp
  LINK_COMPONENTS EmulationCore
  LLVM_COMPONENTS Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationDarwinKernel PRIVATE NOMINMAX)
endif()
