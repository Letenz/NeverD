# Identification needs only the input bytes. Recovery observes a guest process
# on a CPU backend; those cases are registered with the execution tests so
# that the native CPU inventory can require them.
add_neverd_unittest(NeverDUnpackTests
  UnpackIdentifyTests.cpp
  LINK_COMPONENTS Unpack)
target_compile_definitions(NeverDUnpackTests PRIVATE
  NEVERD_UNPACK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/binaries")
