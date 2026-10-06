add_neverd_component_library(NeverDEmulationLinuxKernel
  LinuxServices.cpp
  LinuxTime.cpp
  LinuxClock.cpp
  LinuxSleep.cpp
  LinuxSignals.cpp
  LinuxUserMemory.cpp
  LinuxOutput.cpp
  LinuxServiceABI.cpp
  LinuxThreadPointer.cpp
  LinuxMemory.cpp
  LinuxResidency.cpp
  LinuxFiles.cpp
  LinuxFileOptions.cpp
  LinuxFilePaths.cpp
  LinuxFileIO.cpp
  LinuxFileStatus.cpp
  LinuxFileSystemStatus.cpp
  LINK_COMPONENTS EmulationCore
  LLVM_COMPONENTS Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationLinuxKernel PRIVATE NOMINMAX)
endif()
