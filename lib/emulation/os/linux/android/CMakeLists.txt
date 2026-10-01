add_neverd_component_library(NeverDEmulationAndroid
  AndroidImage.cpp
  AndroidBionic.cpp
  AndroidNative.cpp
  LINK_COMPONENTS EmulationLinux)
if(WIN32)
  target_compile_definitions(NeverDEmulationAndroid PRIVATE NOMINMAX)
endif()
