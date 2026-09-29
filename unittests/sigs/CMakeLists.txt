add_neverd_unittest(NeverDSignatureTests
  MachOPatternGeneratorTests.cpp
  PatternGeneratorTests.cpp
  PatternParserTests.cpp
  RichHeaderTests.cpp
  SignatureCacheTests.cpp
  SignatureDBTests.cpp
  SignatureMatcherTests.cpp
  SignatureTreeTests.cpp
  SynthesizedRoutineTests.cpp

  LINK_COMPONENTS Sigs
)

target_include_directories(NeverDSignatureTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
