if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
set(_deferred_cases
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDeferredCases.def")
file(STRINGS "${_deferred_cases}" _settings
  REGEX "^NEVERD_DEFERRED_(BUILD|TEXT)\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_DEFERRED_(BUILD|TEXT)\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_deferred_${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endif()
endforeach()
set(_deferred_dir "${CMAKE_CURRENT_BINARY_DIR}/${_deferred_OutputDirectory}")
file(MAKE_DIRECTORY "${_deferred_dir}")
# One import library per provider, from the same inventory the fixture uses.
foreach(_provider KERNEL ABSENT)
  if(_provider STREQUAL "KERNEL")
    set(_module "${_deferred_KernelModule}")
  else()
    set(_module "${_deferred_AbsentModule}")
  endif()
  file(STRINGS "${_deferred_cases}" _apis
    REGEX "^NEVERD_DEFERRED_${_provider}_API\\(")
  set(_definition "LIBRARY ${_module}\nEXPORTS\n")
  foreach(_api IN LISTS _apis)
    string(REGEX REPLACE "^NEVERD_DEFERRED_${_provider}_API\\(([A-Za-z0-9_]+)\\)"
      "  \\1\n" _line "${_api}")
    string(APPEND _definition "${_line}")
  endforeach()
  file(CONFIGURE OUTPUT "${_deferred_dir}/${_provider}.def"
    CONTENT "${_definition}" @ONLY)
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_deferred_cases}")
set(_deferred_outputs)
foreach(_arch X64 AArch64)
  set(_dir "${_deferred_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(OUTPUT "${_dir}/${_deferred_ProgramFile}"
      "${_dir}/program.obj" "${_dir}/kernel.lib" "${_dir}/absent.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib
      "/machine:${_deferred_${_arch}Machine}"
      "/def:${_deferred_dir}/KERNEL.def" "/out:${_dir}/kernel.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib
      "/machine:${_deferred_${_arch}Machine}"
      "/def:${_deferred_dir}/ABSENT.def" "/out:${_dir}/absent.lib"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      "--target=${_deferred_${_arch}Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_deferred.c"
      -o "${_dir}/program.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib
      "/entry:${_deferred_ProgramEntry}" /subsystem:console
      "/machine:${_deferred_${_arch}Machine}"
      "/base:${_deferred_ProgramBase}" /timestamp:0
      "${_dir}/program.obj" "${_dir}/kernel.lib" "${_dir}/absent.lib"
      "/out:${_dir}/${_deferred_ProgramFile}"
    DEPENDS fixtures/windows_deferred.c "${_deferred_cases}"
      "${_deferred_dir}/KERNEL.def" "${_deferred_dir}/ABSENT.def"
    VERBATIM)
  list(APPEND _deferred_outputs "${_dir}/${_deferred_ProgramFile}")
  add_custom_command(OUTPUT "${_dir}/${_deferred_GeneratedTLSFile}"
      "${_dir}/${_deferred_GeneratedObjectFile}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      "--target=${_deferred_${_arch}Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_generated_tls.c"
      -o "${_dir}/${_deferred_GeneratedObjectFile}"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib
      "/entry:${_deferred_ProgramEntry}"
      /subsystem:console "/machine:${_deferred_${_arch}Machine}"
      "/base:${_deferred_ProgramBase}" /timestamp:0
      "/section:${_deferred_GeneratedSection},${_deferred_GeneratedSectionPermissions}"
      "${_dir}/${_deferred_GeneratedObjectFile}" "${_dir}/kernel.lib"
      "/out:${_dir}/${_deferred_GeneratedTLSFile}"
    DEPENDS fixtures/windows_generated_tls.c "${_deferred_cases}"
      "${_dir}/kernel.lib"
    VERBATIM)
  list(APPEND _deferred_outputs "${_dir}/${_deferred_GeneratedTLSFile}")
  add_custom_command(OUTPUT "${_dir}/${_deferred_GeneratedTLSEntryFile}"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib
      "/entry:${_deferred_GeneratedEntrySymbol}"
      /subsystem:console "/machine:${_deferred_${_arch}Machine}"
      "/base:${_deferred_ProgramBase}" /timestamp:0
      "/section:${_deferred_GeneratedSection},${_deferred_GeneratedSectionPermissions}"
      "${_dir}/${_deferred_GeneratedObjectFile}" "${_dir}/kernel.lib"
      "/out:${_dir}/${_deferred_GeneratedTLSEntryFile}"
    DEPENDS "${_dir}/${_deferred_GeneratedObjectFile}" "${_dir}/kernel.lib"
    VERBATIM)
  list(APPEND _deferred_outputs "${_dir}/${_deferred_GeneratedTLSEntryFile}")
endforeach()
add_custom_target(NeverDWindowsDeferredFixtures DEPENDS ${_deferred_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsDeferredFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR="${_deferred_dir}")
