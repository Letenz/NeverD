add_neverd_component_library(NeverDEmulationAndroid
  AndroidImage.cpp
  AndroidBionic.cpp
  AndroidStrings.cpp
  AndroidNative.cpp
  LINK_COMPONENTS Loader EmulationRuntime EmulationImage EmulationABI EmulationLinuxKernel
  LLVM_COMPONENTS Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationAndroid PRIVATE NOMINMAX)
endif()
