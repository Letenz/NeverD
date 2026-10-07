add_neverd_component_library(NeverDEmulationLinuxKernel
  LinuxServices.cpp
  LinuxTime.cpp
  LinuxClock.cpp
  LinuxCPUClock.cpp
  LinuxSleep.cpp
  LinuxSignals.cpp
  LinuxPriority.cpp
  LinuxKernelAvailability.cpp
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
  LinuxPIDFD.cpp
  LinuxPoll.cpp
  LinuxFileStatus.cpp
  LinuxFileSystemStatus.cpp
  LinuxFileXAttrs.cpp
  LINK_COMPONENTS EmulationCore
  LLVM_COMPONENTS Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationLinuxKernel PRIVATE NOMINMAX)
endif()
