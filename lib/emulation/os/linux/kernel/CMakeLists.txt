add_neverd_component_library(NeverDEmulationLinuxKernel
  LinuxServices.cpp
  LinuxTime.cpp
  LinuxOutput.cpp
  LinuxServiceABI.cpp
  LinuxThreadPointer.cpp
  LinuxMemory.cpp
  LINK_COMPONENTS EmulationCore
  LLVM_COMPONENTS Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationLinuxKernel PRIVATE NOMINMAX)
endif()
