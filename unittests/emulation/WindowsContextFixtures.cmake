if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsContextCases.def"
  _settings REGEX "^NEVERD_CAPTURE_(BUILD|TEXT)\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_CAPTURE_(BUILD|TEXT)\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_capture_${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endif()
endforeach()
set(_capture_dir "${CMAKE_CURRENT_BINARY_DIR}/${_capture_OutputDirectory}")
file(MAKE_DIRECTORY "${_capture_dir}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsContextCases.def"
  _apis REGEX "^NEVERD_CAPTURE_API")
set(_definition "LIBRARY ${_capture_Kernel}\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_CAPTURE_API\\(([A-Za-z0-9_]+)\\)" "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_capture_dir}/provider.def" CONTENT "${_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsContextCases.def")
set(_capture_outputs)
foreach(_arch X64 AArch64)
  set(_dir "${_capture_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(OUTPUT "${_dir}/provider.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_capture_${_arch}Machine}"
      "/def:${_capture_dir}/provider.def" "/out:${_dir}/provider.lib"
    DEPENDS "${_capture_dir}/provider.def" VERBATIM)
  add_custom_command(OUTPUT "${_dir}/${_capture_ProgramFile}"
      "${_dir}/program.obj" "${_dir}/registers.obj"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_capture_${_arch}Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_context.c" -o "${_dir}/program.obj"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_capture_${_arch}Target}"
      -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_context.S" -o "${_dir}/registers.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /subsystem:console
      "/machine:${_capture_${_arch}Machine}" /timestamp:0 "/entry:${_capture_Entry}"
      "${_dir}/program.obj" "${_dir}/registers.obj" "${_dir}/provider.lib"
      "/out:${_dir}/${_capture_ProgramFile}"
    DEPENDS fixtures/windows_context.c fixtures/windows_context.S
      fixtures/WindowsContextCases.def "${_dir}/provider.lib" VERBATIM)
  list(APPEND _capture_outputs "${_dir}/${_capture_ProgramFile}")
endforeach()
add_custom_target(NeverDWindowsContextFixtures DEPENDS ${_capture_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsContextFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_CONTEXT_FIXTURE_DIR="${_capture_dir}")
