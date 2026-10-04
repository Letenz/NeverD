if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsExceptionCases.def"
  _settings REGEX "^NEVERD_VEH_(BUILD|TEXT)\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_VEH_(BUILD|TEXT)\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_veh_${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endif()
endforeach()
set(_veh_dir "${CMAKE_CURRENT_BINARY_DIR}/${_veh_OutputDirectory}")
file(MAKE_DIRECTORY "${_veh_dir}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsExceptionCases.def"
  _apis REGEX "^NEVERD_VEH_API")
set(_definition "LIBRARY ${_veh_ProviderFile}\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_VEH_API\\(([A-Za-z0-9_]+)\\)" "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_veh_dir}/provider.def" CONTENT "${_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsExceptionCases.def")
set(_veh_outputs)
foreach(_arch X64 AArch64)
  set(_dir "${_veh_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(OUTPUT "${_dir}/provider.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_veh_${_arch}Machine}"
      "/def:${_veh_dir}/provider.def" "/out:${_dir}/provider.lib"
    DEPENDS "${_veh_dir}/provider.def" VERBATIM)
  foreach(_kind program library continuation)
    if(_kind STREQUAL "program")
      set(_source windows_exceptions.c)
      set(_output "${_veh_ProgramFile}")
      set(_link "/entry:${_veh_ProgramEntry}" "/base:${_veh_ProgramBase}")
    elseif(_kind STREQUAL "continuation")
      set(_source windows_continuations.c)
      set(_output "${_veh_ContinueProgramFile}")
      set(_link "/entry:${_veh_ProgramEntry}" "/base:${_veh_ProgramBase}")
    else()
      set(_source windows_exception_dll.c)
      set(_output "${_veh_LibraryFile}")
      set(_link /dll "/entry:${_veh_LibraryEntry}")
    endif()
    add_custom_command(OUTPUT "${_dir}/${_output}" "${_dir}/${_kind}.obj"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_veh_${_arch}Target}"
        -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
        -fno-vectorize -fno-slp-vectorize -O1 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_source}" -o "${_dir}/${_kind}.obj"
      COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /subsystem:console
        "/machine:${_veh_${_arch}Machine}" /timestamp:0 ${_link}
        "${_dir}/${_kind}.obj" "${_dir}/provider.lib" "/out:${_dir}/${_output}"
      DEPENDS "fixtures/${_source}" fixtures/WindowsExceptionCases.def
        fixtures/WindowsContinuationCases.def
        "${_dir}/provider.lib" VERBATIM)
    list(APPEND _veh_outputs "${_dir}/${_output}")
  endforeach()
endforeach()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsAlignmentProcessCases.def"
  _settings REGEX "^NEVERD_ALIGNMENT_PROCESS_TEXT\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_ALIGNMENT_PROCESS_TEXT\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_alignment_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsAlignmentProcessCases.def")
set(_dir "${_veh_dir}/${_alignment_X64Dir}")
add_custom_command(OUTPUT "${_dir}/${_alignment_ProgramFile}"
    "${_dir}/alignment-c.obj" "${_dir}/alignment-S.obj"
  COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_veh_X64Target}"
    -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
    -fno-vectorize -fno-slp-vectorize -O1 -Wall -Wextra -Werror -c
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_alignment_process.c"
    -o "${_dir}/alignment-c.obj"
  COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_veh_X64Target}"
    -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_alignment.S"
    -o "${_dir}/alignment-S.obj"
  COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /subsystem:console
    "/machine:${_veh_X64Machine}" /timestamp:0 "/entry:${_veh_ProgramEntry}"
    "/base:${_veh_ProgramBase}" "${_dir}/alignment-c.obj"
    "${_dir}/alignment-S.obj" "${_dir}/provider.lib"
    "/out:${_dir}/${_alignment_ProgramFile}"
  DEPENDS fixtures/windows_alignment_process.c fixtures/windows_alignment.S
    fixtures/WindowsAlignmentCases.def fixtures/WindowsAlignmentProcessCases.def
    fixtures/WindowsExceptionCases.def "${_dir}/provider.lib" VERBATIM)
list(APPEND _veh_outputs "${_dir}/${_alignment_ProgramFile}")
add_custom_target(NeverDWindowsExceptionFixtures DEPENDS ${_veh_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsExceptionFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR="${_veh_dir}")
