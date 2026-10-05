add_neverd_component_library(NeverDEmulationAndroid
  AndroidImage.cpp
  AndroidBionic.cpp
  AndroidAllocation.cpp
  AndroidDlfcn.cpp
  AndroidKernelCalls.cpp
  AndroidMemoryCalls.cpp
  AndroidOnce.cpp
  AndroidQueries.cpp
  AndroidStrings.cpp
  AndroidArguments.cpp
  AndroidFormatting.cpp
  AndroidScanning.cpp
  AndroidMutex.cpp
  AndroidThreadAttributes.cpp
  AndroidThreads.cpp
  AndroidThreadCalls.cpp
  AndroidMemory.cpp
  AndroidFinalizers.cpp
  AndroidNative.cpp
  LINK_COMPONENTS Loader EmulationRuntime EmulationImage EmulationABI EmulationLinuxKernel
  LLVM_COMPONENTS Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationAndroid PRIVATE NOMINMAX)
endif()
