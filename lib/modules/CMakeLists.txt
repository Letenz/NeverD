add_neverd_component_library(NeverDModules
  ModuleImage.cpp
  ModuleOptionsJSON.cpp
  ModuleSet.cpp
  LINK_COMPONENTS Loader DebugInfo Support
  LLVM_COMPONENTS Support)
