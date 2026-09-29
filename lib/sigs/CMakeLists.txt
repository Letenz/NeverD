add_neverd_component_library(NeverDSigs
  PatternGenerator.cpp
  PatternParser.cpp
  SignatureCache.cpp
  SignatureDB.cpp
  SignatureMatcher.cpp

  LINK_COMPONENTS
  Loader
)
