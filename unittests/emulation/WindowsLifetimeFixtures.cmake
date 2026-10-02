if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
set(_lifetime_dir "${CMAKE_CURRENT_BINARY_DIR}/windows-lifetime-fixtures")
set(_lifetime_outputs)
foreach(_arch X64 AArch64)
  if(_arch STREQUAL "X64")
    set(_target x86_64-pc-windows-msvc)
    set(_machine x64)
  else()
    set(_target aarch64-pc-windows-msvc)
    set(_machine arm64)
  endif()
  set(_dir "${_lifetime_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  set(_compile "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_target}"
    -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
    -fno-vectorize -fno-slp-vectorize -O1 -c)
  set(_kernel "${_windows_fixture_dir}/${_arch}-kernel32.lib")
  foreach(_module leaf middle)
    set(_imports "${_kernel}")
    if(_module STREQUAL "middle")
      list(APPEND _imports "${_dir}/life-leaf.lib")
    endif()
    add_custom_command(OUTPUT "${_dir}/life-${_module}.dll" "${_dir}/life-${_module}.lib" "${_dir}/${_module}.obj"
      COMMAND ${_compile} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_lifetime_${_module}.c"
        -o "${_dir}/${_module}.obj"
      COMMAND "${NEVERD_PROCESS_LLD_LINK}" /dll /entry:dllEntry /nodefaultlib /subsystem:console
        "/machine:${_machine}" /base:0x180000000 /timestamp:0 /include:_tls_used
        "${_dir}/${_module}.obj" ${_imports}
        "/out:${_dir}/life-${_module}.dll" "/implib:${_dir}/life-${_module}.lib"
      DEPENDS "fixtures/windows_lifetime_${_module}.c" fixtures/WindowsLifetimeFixture.h
        fixtures/WindowsLifetimeCases.def ${_imports}
      VERBATIM)
  endforeach()
  add_custom_command(OUTPUT "${_dir}/lifetime.exe" "${_dir}/process.obj"
    COMMAND ${_compile} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_lifetime_process.c"
      -o "${_dir}/process.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /entry:entry /subsystem:console
      "/machine:${_machine}" /base:0x140000000 /include:_tls_used /timestamp:0
      "${_dir}/process.obj" "${_dir}/life-middle.lib" "${_dir}/life-leaf.lib" "${_kernel}"
      "/out:${_dir}/lifetime.exe"
    DEPENDS fixtures/windows_lifetime_process.c fixtures/WindowsLifetimeFixture.h
      fixtures/WindowsLifetimeCases.def "${_dir}/life-middle.lib" "${_dir}/life-leaf.lib" "${_kernel}"
    VERBATIM)
  list(APPEND _lifetime_outputs "${_dir}/lifetime.exe")
endforeach()
add_custom_target(NeverDWindowsLifetimeFixtures DEPENDS ${_lifetime_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsLifetimeFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_LIFETIME_FIXTURE_DIR="${_lifetime_dir}")
