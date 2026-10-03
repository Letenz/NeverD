add_neverd_component_library(NeverDEmulationLinux
  LinuxProcess.cpp
  LinuxProcessLayout.cpp
  LinuxProcessStack.cpp
  LINK_COMPONENTS Loader EmulationRuntime EmulationImage EmulationABI EmulationLinuxKernel
  LLVM_COMPONENTS Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationLinux PRIVATE NOMINMAX)
endif()
