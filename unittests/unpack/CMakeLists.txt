# Identification needs only the input bytes. Recovery observes a guest process
# on a CPU backend; those cases are registered with the execution tests so
# that the native CPU inventory can require them.
add_neverd_unittest(NeverDUnpackTests
  PERebuildTests.cpp
  ProcessTransferTests.cpp
  UnpackIdentifyTests.cpp
  LINK_COMPONENTS Unpack)
target_include_directories(NeverDUnpackTests PRIVATE "${PROJECT_SOURCE_DIR}/lib")
target_compile_definitions(NeverDUnpackTests PRIVATE
  NEVERD_UNPACK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/binaries")
