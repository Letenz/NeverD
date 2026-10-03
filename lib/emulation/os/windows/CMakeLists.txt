# Windows driver execution and its kernel environment share the existing
# component target; each implementation directory owns its source inventory.
add_neverd_component_library(NeverDEmulation
  LINK_COMPONENTS Loader EmulationCPU EmulationABI EmulationRuntime EmulationWindowsException)

add_subdirectory(driver)
add_subdirectory(kernel)

if(WIN32)
  target_compile_definitions(NeverDEmulation PRIVATE NOMINMAX)
endif()
