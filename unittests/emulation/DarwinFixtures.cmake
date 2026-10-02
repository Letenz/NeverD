find_program(NEVERD_PROCESS_LD64 NAMES ld64.lld HINTS "${LLVM_TOOLS_BINARY_DIR}")
if(NOT NEVERD_TEST_CLANG_EXECUTABLE OR NOT NEVERD_PROCESS_LD64)
  message(STATUS "Darwin process fixtures unavailable: Clang and ld64.lld are required")
  return()
endif()
set(_darwin_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/darwin-fixtures")
file(MAKE_DIRECTORY "${_darwin_fixture_dir}")
set(_darwin_outputs)
foreach(_darwin_profile macos ios ios-simulator)
  foreach(_darwin_arch x86_64 arm64)
    if(_darwin_profile STREQUAL "ios" AND _darwin_arch STREQUAL "x86_64")
      continue()
    endif()
    if(_darwin_profile STREQUAL "macos")
      set(_darwin_target "${_darwin_arch}-apple-macos11")
    elseif(_darwin_profile STREQUAL "ios")
      set(_darwin_target "${_darwin_arch}-apple-ios14")
    else()
      set(_darwin_target "${_darwin_arch}-apple-ios14-simulator")
    endif()
    set(_darwin_output "${_darwin_fixture_dir}/${_darwin_profile}-${_darwin_arch}")
    add_custom_command(OUTPUT "${_darwin_output}" "${_darwin_output}.o"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
        -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
        -nostdinc -isysroot "${_darwin_fixture_dir}"
        -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
        -fno-asynchronous-unwind-tables -O1 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_process.c" -o "${_darwin_output}.o"
      COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
        -platform_version "${_darwin_profile}" 14.0 14.0 -e _main
        -no_fixup_chains "${_darwin_output}.o" -o "${_darwin_output}"
      DEPENDS fixtures/darwin_process.c
      VERBATIM)
    list(APPEND _darwin_outputs "${_darwin_output}")
  endforeach()
endforeach()
add_custom_target(NeverDDarwinFixtures DEPENDS ${_darwin_outputs})
add_dependencies(NeverDDarwinProcessTests NeverDDarwinFixtures)
target_compile_definitions(NeverDDarwinProcessTests PRIVATE
  NEVERD_DARWIN_FIXTURE_DIR="${_darwin_fixture_dir}")
if(TARGET NeverDProcessPublicTests)
  add_dependencies(NeverDProcessPublicTests NeverDDarwinFixtures)
  target_compile_definitions(NeverDProcessPublicTests PRIVATE
    NEVERD_DARWIN_FIXTURE_DIR="${_darwin_fixture_dir}")
endif()
