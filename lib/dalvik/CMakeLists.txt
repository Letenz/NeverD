include(${CMAKE_SOURCE_DIR}/cmake/NeverDZlib.cmake)
add_neverd_component_library(NeverDDalvik
  Budget.cpp
  Dex.cpp
  Model.cpp
  Runtime.cpp
  Signature.cpp
  LLVM_COMPONENTS Support
  LINK_LIBS ZLIB::ZLIB)
