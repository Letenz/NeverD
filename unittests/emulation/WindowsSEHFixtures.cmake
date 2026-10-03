if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsSEHCases.def"
  _settings REGEX "^NEVERD_USER_SEH_(BUILD|TEXT)\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_USER_SEH_(BUILD|TEXT)\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_seh_${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endif()
endforeach()
set(_seh_dir "${CMAKE_CURRENT_BINARY_DIR}/${_seh_OutputDirectory}")
file(MAKE_DIRECTORY "${_seh_dir}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsSEHCases.def"
  _apis REGEX "^NEVERD_USER_SEH_API")
set(_definition "LIBRARY ${_seh_Kernel}\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_USER_SEH_API\\(([A-Za-z0-9_]+)\\)" "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_seh_dir}/kernel.def" CONTENT "${_definition}" @ONLY)
file(CONFIGURE OUTPUT "${_seh_dir}/native.def"
  CONTENT "LIBRARY ${_seh_Native}\nEXPORTS\n  ${_seh_Personality}\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsSEHCases.def")
foreach(_provider kernel native)
  add_custom_command(OUTPUT "${_seh_dir}/${_provider}.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_seh_Machine}"
      "/def:${_seh_dir}/${_provider}.def" "/out:${_seh_dir}/${_provider}.lib"
    DEPENDS "${_seh_dir}/${_provider}.def" VERBATIM)
endforeach()
set(_seh_outputs)
foreach(_kind program library)
  if(_kind STREQUAL "program")
    set(_source windows_seh.c)
    set(_output "${_seh_ProgramFile}")
    set(_link "/entry:${_seh_Entry}")
  else()
    set(_source windows_seh_dll.c)
    set(_output "${_seh_LibraryFile}")
    set(_link /dll /noentry)
  endif()
  add_custom_command(OUTPUT "${_seh_dir}/${_output}" "${_seh_dir}/${_kind}.obj"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_seh_Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fms-extensions -fexceptions -fno-vectorize -fno-slp-vectorize -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_source}" -o "${_seh_dir}/${_kind}.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /subsystem:console
      "/machine:${_seh_Machine}" /timestamp:0 "/base:${_seh_Base}" ${_link}
      "${_seh_dir}/${_kind}.obj" "${_seh_dir}/kernel.lib" "${_seh_dir}/native.lib"
      "/out:${_seh_dir}/${_output}"
    DEPENDS "fixtures/${_source}" fixtures/WindowsSEHCases.def
      "${_seh_dir}/kernel.lib" "${_seh_dir}/native.lib" VERBATIM)
  list(APPEND _seh_outputs "${_seh_dir}/${_output}")
endforeach()
add_custom_target(NeverDWindowsSEHFixtures DEPENDS ${_seh_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsSEHFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_SEH_FIXTURE_DIR="${_seh_dir}")
