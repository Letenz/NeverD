if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
set(_windows_export_dir "${CMAKE_CURRENT_BINARY_DIR}/windows-export-fixtures")
file(MAKE_DIRECTORY "${_windows_export_dir}")
foreach(_module leaf bridge top)
  string(TOUPPER "${_module}" _kind)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsExportCases.def"
    _exports REGEX "^NEVERD_EXPORT_${_kind}\\(")
  set(_definition "LIBRARY lookup-${_module}.dll\nEXPORTS\n")
  foreach(_export IN LISTS _exports)
    string(REGEX REPLACE "^NEVERD_EXPORT_${_kind}\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)" "  \\1 \\2\n" _line "${_export}")
    string(APPEND _definition "${_line}")
  endforeach()
  file(CONFIGURE OUTPUT "${_windows_export_dir}/${_module}.def" CONTENT "${_definition}" @ONLY)
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsExportCases.def")
set(_export_outputs)
foreach(_arch X64 AArch64)
  if(_arch STREQUAL "X64")
    set(_target x86_64-pc-windows-msvc)
    set(_machine x64)
  else()
    set(_target aarch64-pc-windows-msvc)
    set(_machine arm64)
  endif()
  set(_dir "${_windows_export_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  set(_compile "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_target}"
    -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
    -fno-vectorize -fno-slp-vectorize -O1 -c)
  set(_kernel "${_windows_fixture_dir}/${_arch}-kernel32.lib")
  foreach(_module leaf bridge top)
    string(TOUPPER "${_module}" _kind)
    add_custom_command(OUTPUT "${_dir}/lookup-${_module}.dll" "${_dir}/lookup-${_module}.lib" "${_dir}/${_module}.obj"
      COMMAND ${_compile} "-DEXPORT_${_kind}"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_export_dll.c" -o "${_dir}/${_module}.obj"
      COMMAND "${NEVERD_PROCESS_LLD_LINK}" /dll /entry:DllMain /nodefaultlib /subsystem:console
        "/machine:${_machine}" /base:0x180000000 /timestamp:0
        "/def:${_windows_export_dir}/${_module}.def"
        "${_dir}/${_module}.obj" "${_kernel}" "/out:${_dir}/lookup-${_module}.dll"
        "/implib:${_dir}/lookup-${_module}.lib"
      DEPENDS fixtures/windows_export_dll.c fixtures/WindowsExportFixture.h
        fixtures/WindowsExportCases.def "${_windows_export_dir}/${_module}.def" "${_kernel}"
      VERBATIM)
    list(APPEND _export_outputs "${_dir}/lookup-${_module}.dll")
  endforeach()
  add_custom_command(OUTPUT "${_dir}/export process.exe" "${_dir}/process.obj"
    COMMAND ${_compile} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_export_process.c"
      -o "${_dir}/process.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /entry:entry /subsystem:console
      "/machine:${_machine}" /base:0x140000000 /timestamp:0
      "${_dir}/process.obj" "${_dir}/lookup-top.lib" "${_kernel}"
      "/out:${_dir}/export process.exe"
    DEPENDS fixtures/windows_export_process.c fixtures/WindowsExportFixture.h
      fixtures/WindowsExportCases.def "${_dir}/lookup-top.lib" "${_kernel}"
    VERBATIM)
  list(APPEND _export_outputs "${_dir}/export process.exe")
endforeach()
add_custom_target(NeverDWindowsExportFixtures DEPENDS ${_export_outputs})
foreach(_owner NeverDWindowsProcessTests NeverDProcessPublicTests)
  if(TARGET ${_owner})
    add_dependencies(${_owner} NeverDWindowsExportFixtures)
    target_compile_definitions(${_owner} PRIVATE NEVERD_WINDOWS_EXPORT_FIXTURE_DIR="${_windows_export_dir}")
  endif()
endforeach()
