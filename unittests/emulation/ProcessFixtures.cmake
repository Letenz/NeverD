find_program(NEVERD_PROCESS_LLD NAMES ld.lld HINTS "${LLVM_TOOLS_BINARY_DIR}")
if(NOT NEVERD_TEST_CLANG_EXECUTABLE OR NOT NEVERD_PROCESS_LLD)
  message(STATUS "ELF process fixtures unavailable: Clang and ld.lld are required")
  return()
endif()
set(_process_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/process-fixtures")
file(MAKE_DIRECTORY "${_process_fixture_dir}")
set(_process_outputs)
foreach(_process_arch X64 AArch64)
  if(_process_arch STREQUAL "X64")
    set(_process_target x86_64-unknown-linux-gnu)
  else()
    set(_process_target aarch64-unknown-linux-gnu)
  endif()
  set(_process_base "${_process_fixture_dir}/${_process_arch}")
  add_custom_command(OUTPUT "${_process_base}.elf" "${_process_base}.o" "${_process_base}-entry.o"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_process_target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/linux_process.c" -o "${_process_base}.o"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_process_target}"
      -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/linux_process.S" -o "${_process_base}-entry.o"
    COMMAND "${NEVERD_PROCESS_LLD}" -static -e _start -z max-page-size=4096
      --build-id=none "${_process_base}.o" "${_process_base}-entry.o" -o "${_process_base}.elf"
    DEPENDS fixtures/linux_process.c fixtures/linux_process.S fixtures/LinuxProcessCases.def
    VERBATIM)
  list(APPEND _process_outputs "${_process_base}.elf")
endforeach()
add_custom_target(NeverDProcessFixtures DEPENDS ${_process_outputs})
add_dependencies(NeverDLinuxProcessTests NeverDProcessFixtures)
target_compile_definitions(NeverDLinuxProcessTests PRIVATE
  NEVERD_PROCESS_FIXTURE_DIR="${_process_fixture_dir}")
if(TARGET NeverDProcessPublicTests)
  add_dependencies(NeverDProcessPublicTests NeverDProcessFixtures)
  target_compile_definitions(NeverDProcessPublicTests PRIVATE
    NEVERD_PROCESS_FIXTURE_DIR="${_process_fixture_dir}")
endif()
