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
  add_custom_command(OUTPUT "${_process_base}-tls.elf" "${_process_base}-tls.o"
      "${_process_base}-tls-pointer.o"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_process_target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables -ftls-model=local-exec -fno-pic -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/linux_tls.c" -o "${_process_base}-tls.o"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_process_target}"
      -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/linux_tls.S" -o "${_process_base}-tls-pointer.o"
    COMMAND "${NEVERD_PROCESS_LLD}" -static -e _start -z max-page-size=4096
      --build-id=none "${_process_base}-tls.o" "${_process_base}-entry.o"
      "${_process_base}-tls-pointer.o" -o "${_process_base}-tls.elf"
    DEPENDS "${_process_base}.elf" fixtures/linux_tls.c fixtures/linux_tls.S
      fixtures/LinuxTLSCases.def fixtures/LinuxProcessCases.def
    VERBATIM)
  list(APPEND _process_outputs "${_process_base}-tls.elf")
  add_custom_command(OUTPUT "${_process_base}-pie.elf" "${_process_base}-pie.o"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_process_target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables -fPIE -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/linux_pie.c" -o "${_process_base}-pie.o"
    COMMAND "${NEVERD_PROCESS_LLD}" -static -pie --no-dynamic-linker -e _start
      -z max-page-size=4096 --build-id=none "${_process_base}-pie.o"
      "${_process_base}-entry.o" -o "${_process_base}-pie.elf"
    DEPENDS "${_process_base}.elf" fixtures/linux_pie.c
      fixtures/LinuxPIECases.def fixtures/LinuxProcessCases.def
    VERBATIM)
  list(APPEND _process_outputs "${_process_base}-pie.elf")
  add_custom_command(OUTPUT "${_process_base}-memory.elf" "${_process_base}-memory.o"
      "${_process_base}-memory-call.o"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_process_target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/linux_memory.c" -o "${_process_base}-memory.o"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_process_target}"
      -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/linux_memory.S" -o "${_process_base}-memory-call.o"
    COMMAND "${NEVERD_PROCESS_LLD}" -static -e _start -z max-page-size=4096
      --build-id=none "${_process_base}-memory.o" "${_process_base}-entry.o"
      "${_process_base}-memory-call.o" -o "${_process_base}-memory.elf"
    DEPENDS "${_process_base}.elf" fixtures/linux_memory.c fixtures/linux_memory.S
      fixtures/LinuxMemoryCases.def
    VERBATIM)
  list(APPEND _process_outputs "${_process_base}-memory.elf")
  add_custom_command(OUTPUT "${_process_base}-output.elf" "${_process_base}-output.o"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_process_target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/linux_output.c" -o "${_process_base}-output.o"
    COMMAND "${NEVERD_PROCESS_LLD}" -static -e _start -z max-page-size=4096
      --build-id=none "${_process_base}-output.o" "${_process_base}-entry.o"
      -o "${_process_base}-output.elf"
    DEPENDS "${_process_base}.elf" fixtures/linux_output.c fixtures/LinuxOutputCases.def
    VERBATIM)
  list(APPEND _process_outputs "${_process_base}-output.elf")
  foreach(_optimization O0 O2)
    foreach(_file_kind time files signals residency priority kernel)
      set(_service_base "${_process_base}-${_file_kind}-${_optimization}")
      add_custom_command(OUTPUT "${_service_base}.elf" "${_service_base}.o"
        COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_process_target}"
          -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
          -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
          -fno-asynchronous-unwind-tables "-${_optimization}" -c
          "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/linux_${_file_kind}.c" -o "${_service_base}.o"
        COMMAND "${NEVERD_PROCESS_LLD}" -static -e _start -z max-page-size=4096
          --build-id=none "${_service_base}.o" "${_process_base}-entry.o" -o "${_service_base}.elf"
        DEPENDS "${_process_base}.elf" "fixtures/linux_${_file_kind}.c" VERBATIM)
      list(APPEND _process_outputs "${_service_base}.elf")
    endforeach()
  endforeach()
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
