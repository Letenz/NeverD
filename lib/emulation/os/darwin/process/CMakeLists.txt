add_neverd_component_library(NeverDEmulationDarwin
  DarwinProcess.cpp
  DarwinImage.cpp
  DarwinCallbacks.cpp
  DarwinLibSystem.cpp
  DarwinTLS.cpp
  DarwinTLSPreparation.cpp
  DarwinStack.cpp
  ../macos/MacOSProfile.cpp
  ../ios/IOSProfile.cpp
  LINK_COMPONENTS EmulationRuntime EmulationABI EmulationImage EmulationDarwinKernel
  LLVM_COMPONENTS Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationDarwin PRIVATE NOMINMAX)
endif()
