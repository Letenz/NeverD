add_neverd_component_library(NeverDSigs
  LibraryFeature.cpp
  LibraryRecognition.cpp
  LibraryByteRecognition.cpp
  PatternGenerator.cpp
  PatternParser.cpp
  SignatureCache.cpp
  SignatureDB.cpp
  SignatureMatcher.cpp

  LINK_COMPONENTS
  Loader
)
