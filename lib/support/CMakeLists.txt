add_neverd_component_library(NeverDDigest
  SHA256.cpp
  LLVM_COMPONENTS Support TargetParser
)

add_neverd_component_library(NeverDSupport
  AtomicOutput.cpp
  BinaryLoading.cpp
  StringScan.cpp
  TextCodec.cpp
  TextEncoding.cpp
  TextEvidence.cpp

  LINK_COMPONENTS
  Loader
)
