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
set(_android_outputs)
foreach(_packing none android relr)
  set(_android_file "${_android_fixture_dir}/${_packing}.so")
  add_custom_command(OUTPUT "${_android_file}"
    COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
      --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
      "${_android_object}" -o "${_android_file}"
    DEPENDS "${_android_object}" VERBATIM)
  list(APPEND _android_outputs "${_android_file}")
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
