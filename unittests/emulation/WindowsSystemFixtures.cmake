if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsSystemCases.def"
  _settings REGEX "^NEVERD_SYSTEM_(BUILD|TEXT)\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_SYSTEM_(BUILD|TEXT)\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_system_${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endif()
endforeach()
set(_system_dir "${CMAKE_CURRENT_BINARY_DIR}/${_system_OutputDirectory}")
file(MAKE_DIRECTORY "${_system_dir}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsSystemCases.def"
  _apis REGEX "^NEVERD_SYSTEM_API")
set(_definition "LIBRARY ${_system_ProviderFile}\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_SYSTEM_API\\(([A-Za-z0-9_]+)\\)" "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_system_dir}/provider.def" CONTENT "${_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsSystemCases.def")
file(CONFIGURE OUTPUT "${_system_dir}/forward.def"
  CONTENT "LIBRARY ${_system_ForwardFile}\nEXPORTS\n  ${_system_ForwardDefinition}\n" @ONLY)
set(_system_outputs)
foreach(_arch X64 AArch64)
  set(_dir "${_system_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(OUTPUT "${_dir}/${_system_ProgramFile}" "${_dir}/program.obj"
      "${_dir}/provider.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_system_${_arch}Machine}"
      "/def:${_system_dir}/provider.def" "/out:${_dir}/provider.lib"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_system_${_arch}Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_system.c" -o "${_dir}/program.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib "/entry:${_system_ProgramEntry}"
      /subsystem:console "/machine:${_system_${_arch}Machine}"
      "/base:${_system_ProgramBase}" /timestamp:0
      "${_dir}/program.obj" "${_dir}/provider.lib" "/out:${_dir}/${_system_ProgramFile}"
    DEPENDS fixtures/windows_system.c fixtures/WindowsSystemCases.def
      fixtures/WindowsSectionFixture.inc
      fixtures/WindowsCriticalSectionFixture.inc
      fixtures/WindowsNativeServiceFixture.inc
      fixtures/WindowsFLSExitFixture.inc
      "${_system_dir}/provider.def" VERBATIM)
  add_custom_command(OUTPUT "${_dir}/${_system_ForwardFile}" "${_dir}/forward.obj"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_system_${_arch}Target}"
      -std=c11 -ffreestanding -fno-stack-protector -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_system_forward.c"
      -o "${_dir}/forward.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /dll /noentry /nodefaultlib /subsystem:console
      "/machine:${_system_${_arch}Machine}" /timestamp:0
      "/def:${_system_dir}/forward.def" "${_dir}/forward.obj" "${_dir}/provider.lib"
      "/out:${_dir}/${_system_ForwardFile}"
    DEPENDS "${_system_dir}/forward.def" "${_dir}/provider.lib"
      fixtures/windows_system_forward.c VERBATIM)
  list(APPEND _system_outputs "${_dir}/${_system_ProgramFile}" "${_dir}/${_system_ForwardFile}")
endforeach()
add_custom_target(NeverDWindowsSystemFixtures DEPENDS ${_system_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsSystemFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_SYSTEM_FIXTURE_DIR="${_system_dir}")
