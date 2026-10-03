# User process policy is available without the Windows driver environment.
add_neverd_component_library(NeverDEmulationWindowsProcess
  WindowsProcess.cpp
  WindowsProcessContext.cpp
  WindowsProcessExceptions.cpp
  WindowsProcessImage.cpp
  WindowsProcessModules.cpp
  WindowsSystemModules.cpp
  WindowsProcessExports.cpp
  WindowsProcessLifetime.cpp
  WindowsProcessLoader.cpp
  WindowsProcessEnvironment.cpp
  WindowsProcessVariables.cpp
  WindowsProcessHeap.cpp
  WindowsProcessServices.cpp
  WindowsProcessMemory.cpp
  LINK_COMPONENTS Loader EmulationRuntime EmulationABI
  LLVM_COMPONENTS Object Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationWindowsProcess PRIVATE NOMINMAX)
endif()
