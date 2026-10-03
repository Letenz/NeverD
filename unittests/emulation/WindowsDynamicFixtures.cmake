if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
set(_dynamic_dir "${CMAKE_CURRENT_BINARY_DIR}/windows-dynamic-fixtures")
file(MAKE_DIRECTORY "${_dynamic_dir}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDynamicCases.def"
  _exports REGEX "^NEVERD_DYNAMIC_TOP_EXPORT")
set(_definition "LIBRARY dynamic-top.dll\nEXPORTS\n")
foreach(_export IN LISTS _exports)
  string(REGEX REPLACE "^NEVERD_DYNAMIC_TOP_EXPORT\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)" "  \\1 \\2\n" _line "${_export}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_dynamic_dir}/top.def" CONTENT "${_definition}" @ONLY)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDynamicCases.def"
  _exports REGEX "^NEVERD_DYNAMIC_MIDDLE_EXPORT")
set(_definition "LIBRARY dynamic-middle.dll\nEXPORTS\n")
foreach(_export IN LISTS _exports)
  string(REGEX REPLACE "^NEVERD_DYNAMIC_MIDDLE_EXPORT\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)" "  \\1 \\2\n" _line "${_export}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_dynamic_dir}/middle.def" CONTENT "${_definition}" @ONLY)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDynamicCases.def"
  _apis REGEX "^NEVERD_DYNAMIC_API")
set(_definition "LIBRARY kernel32.dll\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_DYNAMIC_API\\(([A-Za-z0-9_]+)\\)" "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_dynamic_dir}/kernel32.def" CONTENT "${_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDynamicCases.def")
set(_dynamic_outputs)
foreach(_arch X64 AArch64)
  if(_arch STREQUAL "X64")
    set(_target x86_64-pc-windows-msvc)
    set(_machine x64)
  else()
    set(_target aarch64-pc-windows-msvc)
    set(_machine arm64)
  endif()
  set(_dir "${_dynamic_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}/noentry")
  set(_compile "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_target}"
    -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
    -fno-vectorize -fno-slp-vectorize -O1 -c)
  set(_kernel "${_windows_fixture_dir}/${_arch}-kernel32.lib")
  set(_dynamic_kernel "${_dir}/dynamic-kernel32.lib")
  add_custom_command(OUTPUT "${_dynamic_kernel}"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_machine}"
      "/def:${_dynamic_dir}/kernel32.def" "/out:${_dynamic_kernel}"
    DEPENDS "${_dynamic_dir}/kernel32.def" VERBATIM)
  foreach(_module leaf middle top)
    set(_imports "${_kernel}" "${_dynamic_kernel}")
    set(_link /include:_tls_used)
    if(_module STREQUAL "middle")
      list(APPEND _link "/def:${_dynamic_dir}/middle.def")
      list(APPEND _imports "${_dir}/dynamic-leaf.lib")
    elseif(_module STREQUAL "top")
      set(_link "/def:${_dynamic_dir}/top.def")
    endif()
    add_custom_command(OUTPUT "${_dir}/dynamic-${_module}.dll" "${_dir}/dynamic-${_module}.lib" "${_dir}/${_module}.obj"
      COMMAND ${_compile} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_dynamic_${_module}.c"
        -o "${_dir}/${_module}.obj"
      COMMAND "${NEVERD_PROCESS_LLD_LINK}" /dll /entry:dllEntry /nodefaultlib /subsystem:console
        "/machine:${_machine}" /base:0x180000000 /timestamp:0 ${_link}
        "${_dir}/${_module}.obj" ${_imports}
        "/out:${_dir}/dynamic-${_module}.dll" "/implib:${_dir}/dynamic-${_module}.lib"
      DEPENDS "fixtures/windows_dynamic_${_module}.c" fixtures/WindowsDynamicFixture.h
        fixtures/WindowsDynamicCases.def "${_dynamic_dir}/top.def" "${_dynamic_dir}/middle.def" ${_imports}
      VERBATIM)
    if(NOT _module STREQUAL "top")
      add_custom_command(OUTPUT "${_dir}/noentry/dynamic-${_module}.dll"
        COMMAND "${NEVERD_PROCESS_LLD_LINK}" /dll /noentry /nodefaultlib /subsystem:console
          "/machine:${_machine}" /base:0x180000000 /timestamp:0 ${_link}
          "${_dir}/${_module}.obj" ${_imports}
          "/out:${_dir}/noentry/dynamic-${_module}.dll"
        DEPENDS "${_dir}/${_module}.obj" ${_imports} VERBATIM)
      list(APPEND _dynamic_outputs "${_dir}/noentry/dynamic-${_module}.dll")
    endif()
    list(APPEND _dynamic_outputs "${_dir}/dynamic-${_module}.dll")
  endforeach()
  foreach(_program dynamic dynamic-static)
    set(_flags)
    set(_imports "${_kernel}" "${_dynamic_kernel}")
    if(_program STREQUAL "dynamic-static")
      list(APPEND _flags -DNEVERD_DYNAMIC_STATIC)
      list(APPEND _imports "${_dir}/dynamic-middle.lib")
    endif()
    add_custom_command(OUTPUT "${_dir}/${_program}.exe" "${_dir}/${_program}.obj"
      COMMAND ${_compile} ${_flags} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_dynamic_process.c"
        -o "${_dir}/${_program}.obj"
      COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /entry:entry /subsystem:console
        "/machine:${_machine}" /base:0x140000000 /timestamp:0
        "${_dir}/${_program}.obj" ${_imports} "/out:${_dir}/${_program}.exe"
      DEPENDS fixtures/windows_dynamic_process.c fixtures/WindowsDynamicFixture.h
        fixtures/WindowsDynamicCases.def ${_imports} VERBATIM)
    list(APPEND _dynamic_outputs "${_dir}/${_program}.exe")
  endforeach()
endforeach()
add_custom_target(NeverDWindowsDynamicFixtures DEPENDS ${_dynamic_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsDynamicFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR="${_dynamic_dir}")
