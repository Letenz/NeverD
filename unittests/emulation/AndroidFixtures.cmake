if(NOT NEVERD_TEST_CLANG_EXECUTABLE OR NOT NEVERD_PROCESS_LLD)
  message(STATUS "Android native fixtures unavailable: Clang and ld.lld required")
  return()
endif()
set(_android_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/android-fixtures")
file(MAKE_DIRECTORY "${_android_fixture_dir}")
set(_android_object "${_android_fixture_dir}/native.o")
add_custom_command(OUTPUT "${_android_object}"
  COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
    -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
    -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
    -fno-asynchronous-unwind-tables -O1 -c
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_native.c" -o "${_android_object}"
  DEPENDS fixtures/android_native.c VERBATIM)
set(_android_output_object "${_android_fixture_dir}/output.o")
add_custom_command(OUTPUT "${_android_output_object}"
  COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
    -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
    -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
    -fno-asynchronous-unwind-tables -O1 -c
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_output.c" -o "${_android_output_object}"
  DEPENDS fixtures/android_output.c fixtures/LinuxOutputCases.def VERBATIM)
set(_android_outputs)
foreach(_packing none android relr)
  set(_android_file "${_android_fixture_dir}/${_packing}.so")
  add_custom_command(OUTPUT "${_android_file}"
    COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
      --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
      "${_android_object}" "${_android_output_object}" -o "${_android_file}"
    DEPENDS "${_android_object}" "${_android_output_object}" VERBATIM)
  list(APPEND _android_outputs "${_android_file}")
endforeach()
foreach(_optimization O0 O2)
  set(_once_object "${_android_fixture_dir}/once-${_optimization}.o")
  add_custom_command(OUTPUT "${_once_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_once.c" -o "${_once_object}"
    DEPENDS fixtures/android_once.c VERBATIM)
  foreach(_packing none android relr)
    set(_once_file "${_android_fixture_dir}/once-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_once_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_once_object}" -o "${_once_file}"
      DEPENDS "${_once_object}" VERBATIM)
    list(APPEND _android_outputs "${_once_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_token_object "${_android_fixture_dir}/token-${_optimization}.o")
  add_custom_command(OUTPUT "${_token_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_token.c" -o "${_token_object}"
    DEPENDS fixtures/android_token.c VERBATIM)
  foreach(_packing none android relr)
    set(_token_file "${_android_fixture_dir}/token-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_token_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_token_object}" -o "${_token_file}"
      DEPENDS "${_token_object}" VERBATIM)
    list(APPEND _android_outputs "${_token_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_android_mutex_object "${_android_fixture_dir}/mutex-${_optimization}.o")
  add_custom_command(OUTPUT "${_android_mutex_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_mutex.c"
      -o "${_android_mutex_object}"
    DEPENDS fixtures/android_mutex.c VERBATIM)
  foreach(_packing none android relr)
    set(_android_mutex_file "${_android_fixture_dir}/mutex-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_android_mutex_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_android_mutex_object}" -o "${_android_mutex_file}"
      DEPENDS "${_android_mutex_object}" VERBATIM)
    list(APPEND _android_outputs "${_android_mutex_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_android_syscall_object "${_android_fixture_dir}/syscall-${_optimization}.o")
  add_custom_command(OUTPUT "${_android_syscall_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_syscall.c"
      -o "${_android_syscall_object}"
    DEPENDS fixtures/android_syscall.c VERBATIM)
  foreach(_packing none android relr)
    set(_android_syscall_file "${_android_fixture_dir}/syscall-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_android_syscall_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_android_syscall_object}" -o "${_android_syscall_file}"
      DEPENDS "${_android_syscall_object}" VERBATIM)
    list(APPEND _android_outputs "${_android_syscall_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_time_object "${_android_fixture_dir}/time-${_optimization}.o")
  add_custom_command(OUTPUT "${_time_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_time.c" -o "${_time_object}"
    DEPENDS fixtures/android_time.c VERBATIM)
  foreach(_packing none android relr)
    set(_time_file "${_android_fixture_dir}/time-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_time_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_time_object}" -o "${_time_file}"
      DEPENDS "${_time_object}" VERBATIM)
    list(APPEND _android_outputs "${_time_file}")
  endforeach()
endforeach()
add_custom_target(NeverDAndroidFixtures DEPENDS ${_android_outputs})
add_dependencies(NeverDAndroidNativeTests NeverDAndroidFixtures)
target_compile_definitions(NeverDAndroidNativeTests PRIVATE
  NEVERD_ANDROID_FIXTURE_DIR="${_android_fixture_dir}")
if(TARGET NeverDProcessPublicTests)
  add_dependencies(NeverDProcessPublicTests NeverDAndroidFixtures)
  target_compile_definitions(NeverDProcessPublicTests PRIVATE
    NEVERD_ANDROID_FIXTURE_DIR="${_android_fixture_dir}")
endif()
