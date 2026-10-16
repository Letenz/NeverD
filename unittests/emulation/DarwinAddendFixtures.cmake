# Original ordinary and TLS pointer initializers with signed dylib addends.
set(_darwin_addend_outputs)
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
    set(_darwin_prefix "${_darwin_fixture_dir}/${_darwin_profile}-${_darwin_arch}-dylib-addends")
    foreach(_darwin_unit main provider data pointers)
      if(_darwin_unit STREQUAL "main")
        set(_darwin_source darwin_dylib_addends.c)
      elseif(_darwin_unit STREQUAL "provider")
        set(_darwin_source darwin_dylib_addend_provider.c)
      else()
        set(_darwin_source "darwin_dylib_addend_${_darwin_unit}.S")
      endif()
      add_custom_command(OUTPUT "${_darwin_prefix}-${_darwin_unit}.o"
        COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
          -ffreestanding -fno-builtin -fno-stack-protector -nostdinc
          -isysroot "${_darwin_fixture_dir}"
          -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
          -fno-asynchronous-unwind-tables -O1 -c
          "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_darwin_source}"
          -o "${_darwin_prefix}-${_darwin_unit}.o"
        DEPENDS "fixtures/${_darwin_source}" VERBATIM)
    endforeach()
    foreach(_darwin_kind classic chained)
      if(_darwin_kind STREQUAL "classic")
        set(_darwin_flag -no_fixup_chains)
      else()
        set(_darwin_flag -fixup_chains)
      endif()
      set(_darwin_dir "${_darwin_prefix}-${_darwin_kind}")
      file(MAKE_DIRECTORY "${_darwin_dir}")
      set(_darwin_library "${_darwin_dir}/libneverd-addends.dylib")
      set(_darwin_system "${_darwin_fixture_dir}/dylib-libsystem-${_darwin_kind}.tbd")
      add_custom_command(OUTPUT "${_darwin_library}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag}
          -dylib -install_name "@loader_path/libneverd-addends.dylib"
          -current_version 1.2 -compatibility_version 1.0
          "${_darwin_prefix}-data.o" "${_darwin_prefix}-provider.o"
          "${_darwin_system}" -o "${_darwin_library}"
        DEPENDS "${_darwin_prefix}-data.o" "${_darwin_prefix}-provider.o"
          "${_darwin_system}" VERBATIM)
      add_custom_command(OUTPUT "${_darwin_dir}/main"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag} -e _main
          "${_darwin_prefix}-main.o" "${_darwin_prefix}-pointers.o"
          "${_darwin_library}" "${_darwin_system}" -o "${_darwin_dir}/main"
        DEPENDS "${_darwin_prefix}-main.o" "${_darwin_prefix}-pointers.o"
          "${_darwin_library}" "${_darwin_system}" VERBATIM)
      list(APPEND _darwin_addend_outputs "${_darwin_dir}/main")
    endforeach()
  endforeach()
endforeach()
add_custom_target(NeverDDarwinAddendFixtures DEPENDS ${_darwin_addend_outputs})
add_dependencies(NeverDDarwinProcessTests NeverDDarwinAddendFixtures)
if(TARGET NeverDProcessPublicTests)
  add_dependencies(NeverDProcessPublicTests NeverDDarwinAddendFixtures)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_CROSSCOMPILING)
  set(_darwin_prefix "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-dylib-addends")
  foreach(_darwin_kind classic chained)
    if(_darwin_kind STREQUAL "classic")
      set(_darwin_flag -Wl,-no_fixup_chains)
    else()
      set(_darwin_flag -Wl,-fixup_chains)
    endif()
    set(_darwin_dir "${_darwin_fixture_dir}/macos-dylib-addends-native-${_darwin_kind}")
    file(MAKE_DIRECTORY "${_darwin_dir}")
    set(_darwin_library "${_darwin_dir}/libneverd-addends.dylib")
    add_custom_command(OUTPUT "${_darwin_library}"
      COMMAND xcrun clang -arch "${_darwin_native_arch}" -dynamiclib
        ${_darwin_flag} -Wl,-install_name,@loader_path/libneverd-addends.dylib
        -Wl,-current_version,1.2 -Wl,-compatibility_version,1.0
        "${_darwin_prefix}-data.o" "${_darwin_prefix}-provider.o"
        -o "${_darwin_library}"
      DEPENDS "${_darwin_prefix}-data.o" "${_darwin_prefix}-provider.o" VERBATIM)
    add_custom_command(OUTPUT "${_darwin_dir}/main"
      COMMAND xcrun clang -arch "${_darwin_native_arch}" ${_darwin_flag}
        "${_darwin_prefix}-main.o" "${_darwin_prefix}-pointers.o"
        "${_darwin_library}" -o "${_darwin_dir}/main"
      DEPENDS "${_darwin_prefix}-main.o" "${_darwin_prefix}-pointers.o"
        "${_darwin_library}" VERBATIM)
    add_custom_target(NeverDDarwinAddendNative_${_darwin_kind}
      DEPENDS "${_darwin_dir}/main")
    add_dependencies(NeverDDarwinProcessTests NeverDDarwinAddendNative_${_darwin_kind})
    string(TOUPPER "${_darwin_kind}" _darwin_macro)
    target_compile_definitions(NeverDDarwinProcessTests PRIVATE
      "NEVERD_DARWIN_ADDENDS_NATIVE_${_darwin_macro}=\"${_darwin_dir}/main\"")
  endforeach()
endif()
