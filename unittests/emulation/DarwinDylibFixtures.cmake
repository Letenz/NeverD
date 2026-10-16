# Original, SDK-free startup dependency graph for all five Darwin profiles.
set(_darwin_dylib_outputs)
foreach(_darwin_kind classic chained)
  set(_darwin_files_binder "")
  if(_darwin_kind STREQUAL "classic")
    set(_darwin_files_binder ", dyld_stub_binder")
  endif()
  configure_file(fixtures/darwin_dylib_libsystem.tbd.in
    "${_darwin_fixture_dir}/dylib-libsystem-${_darwin_kind}.tbd" @ONLY)
endforeach()
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
    set(_darwin_prefix "${_darwin_fixture_dir}/${_darwin_profile}-${_darwin_arch}-dylib")
    foreach(_darwin_unit base leaf main)
      add_custom_command(OUTPUT "${_darwin_prefix}-${_darwin_unit}.o"
        COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
          -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -nostdinc
          -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
          -fno-asynchronous-unwind-tables -O1 -c
          "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_dylib_${_darwin_unit}.c"
          -o "${_darwin_prefix}-${_darwin_unit}.o"
        DEPENDS "fixtures/darwin_dylib_${_darwin_unit}.c" VERBATIM)
    endforeach()
    foreach(_darwin_kind classic chained)
      if(_darwin_kind STREQUAL "classic")
        set(_darwin_flag -no_fixup_chains)
      else()
        set(_darwin_flag -fixup_chains)
      endif()
      set(_darwin_dir "${_darwin_prefix}-${_darwin_kind}")
      file(MAKE_DIRECTORY "${_darwin_dir}")
      set(_darwin_base "${_darwin_dir}/libneverd-base.dylib")
      set(_darwin_leaf "${_darwin_dir}/libneverd-leaf.dylib")
      set(_darwin_system "${_darwin_fixture_dir}/dylib-libsystem-${_darwin_kind}.tbd")
      add_custom_command(OUTPUT "${_darwin_base}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag}
          -dylib -install_name "@loader_path/libneverd-base.dylib"
          -current_version 1.2 -compatibility_version 1.0
          "${_darwin_prefix}-base.o" "${_darwin_system}"
          -o "${_darwin_base}"
        DEPENDS "${_darwin_prefix}-base.o"
          "${_darwin_system}" VERBATIM)
      add_custom_command(OUTPUT "${_darwin_leaf}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag}
          -dylib -install_name "@loader_path/libneverd-leaf.dylib"
          -current_version 1.2 -compatibility_version 1.0
          "${_darwin_prefix}-leaf.o" "${_darwin_base}" "${_darwin_system}"
          -o "${_darwin_leaf}"
        DEPENDS "${_darwin_prefix}-leaf.o" "${_darwin_base}" "${_darwin_system}"
        VERBATIM)
      add_custom_command(OUTPUT "${_darwin_dir}/main"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag} -e _main
          "${_darwin_prefix}-main.o" "${_darwin_base}" "${_darwin_leaf}"
          "${_darwin_system}" -o "${_darwin_dir}/main"
        DEPENDS "${_darwin_prefix}-main.o" "${_darwin_base}"
          "${_darwin_leaf}" "${_darwin_system}" VERBATIM)
      list(APPEND _darwin_dylib_outputs "${_darwin_dir}/main")
    endforeach()
  endforeach()
endforeach()
add_custom_target(NeverDDarwinDylibFixtures DEPENDS ${_darwin_dylib_outputs})
add_dependencies(NeverDDarwinProcessTests NeverDDarwinDylibFixtures)
if(TARGET NeverDProcessPublicTests)
  add_dependencies(NeverDProcessPublicTests NeverDDarwinDylibFixtures)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_CROSSCOMPILING)
  set(_darwin_dylib_native_dir "${_darwin_fixture_dir}/macos-dylib-native")
  file(MAKE_DIRECTORY "${_darwin_dylib_native_dir}")
  set(_darwin_dylib_native_prefix "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-dylib")
  set(_darwin_dylib_native_base "${_darwin_dylib_native_dir}/libneverd-base.dylib")
  set(_darwin_dylib_native_leaf "${_darwin_dylib_native_dir}/libneverd-leaf.dylib")
  add_custom_command(OUTPUT "${_darwin_dylib_native_base}"
    COMMAND xcrun clang -arch "${_darwin_native_arch}" -dynamiclib
      -Wl,-no_fixup_chains -Wl,-install_name,@loader_path/libneverd-base.dylib
      -Wl,-current_version,1.2 -Wl,-compatibility_version,1.0
      "${_darwin_dylib_native_prefix}-base.o" -o "${_darwin_dylib_native_base}"
    DEPENDS "${_darwin_dylib_native_prefix}-base.o" VERBATIM)
  add_custom_command(OUTPUT "${_darwin_dylib_native_leaf}"
    COMMAND xcrun clang -arch "${_darwin_native_arch}" -dynamiclib
      -Wl,-no_fixup_chains -Wl,-install_name,@loader_path/libneverd-leaf.dylib
      -Wl,-current_version,1.2 -Wl,-compatibility_version,1.0
      "${_darwin_dylib_native_prefix}-leaf.o" "${_darwin_dylib_native_base}"
      -o "${_darwin_dylib_native_leaf}"
    DEPENDS "${_darwin_dylib_native_prefix}-leaf.o" "${_darwin_dylib_native_base}"
    VERBATIM)
  set(_darwin_dylib_native "${_darwin_dylib_native_dir}/main")
  add_custom_command(OUTPUT "${_darwin_dylib_native}"
    COMMAND xcrun clang -arch "${_darwin_native_arch}" -Wl,-no_fixup_chains
      "${_darwin_dylib_native_prefix}-main.o"
      "${_darwin_dylib_native_base}" "${_darwin_dylib_native_leaf}"
      -o "${_darwin_dylib_native}"
    DEPENDS "${_darwin_dylib_native_prefix}-main.o"
      "${_darwin_dylib_native_base}" "${_darwin_dylib_native_leaf}" VERBATIM)
  add_custom_target(NeverDDarwinDylibNativeOracle DEPENDS "${_darwin_dylib_native}")
  add_dependencies(NeverDDarwinProcessTests NeverDDarwinDylibNativeOracle)
  target_compile_definitions(NeverDDarwinProcessTests PRIVATE
    NEVERD_DARWIN_DYLIB_NATIVE_ORACLE="${_darwin_dylib_native}")
endif()
