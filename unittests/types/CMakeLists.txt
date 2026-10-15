add_neverd_unittest(NeverDTypesTests
  LocalTypesTests.cpp
  PDBTypeImportTests.cpp
  TypeRenderTests.cpp

  LINK_COMPONENTS
  Types
)

target_include_directories(NeverDTypesTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(NeverDTypesTests PRIVATE
  NEVERD_TYPES_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
