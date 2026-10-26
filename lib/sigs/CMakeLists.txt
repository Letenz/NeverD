add_neverd_component_library(NeverDSigs
  LibraryFeature.cpp
  LibraryRecognition.cpp
  LibraryByteRecognition.cpp
  PatternGenerator.cpp
  VTableEvidence.cpp
  PatternParser.cpp
  SignatureCache.cpp
  SignatureDB.cpp
  SignatureDataContext.cpp
  SignatureVTableContext.cpp
  SignatureMatcher.cpp

  LINK_COMPONENTS
  Loader
)
