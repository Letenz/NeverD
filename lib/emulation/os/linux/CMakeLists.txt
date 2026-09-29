add_neverd_component_library(NeverDEmulationLinux
  LinuxProcess.cpp
  LinuxProcessLayout.cpp
  LinuxProcessStack.cpp
  LinuxServiceABI.cpp
  LinuxThreadPointer.cpp
  LinuxMemory.cpp
  LINK_COMPONENTS Loader EmulationRuntime EmulationImage EmulationABI)
if(WIN32)
  target_compile_definitions(NeverDEmulationLinux PRIVATE NOMINMAX)
endif()
