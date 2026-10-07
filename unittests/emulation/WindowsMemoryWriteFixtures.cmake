if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsMemoryWriteCases.def"
  _settings REGEX "^NEVERD_MEMORY_WRITE_TEXT\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_MEMORY_WRITE_TEXT\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_memory_write_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()
set(_memory_write_dir "${CMAKE_CURRENT_BINARY_DIR}/${_memory_write_OutputDirectory}")
file(MAKE_DIRECTORY "${_memory_write_dir}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsMemoryWriteCases.def"
  _apis REGEX "^NEVERD_MEMORY_WRITE_API")
set(_definition "LIBRARY ${_memory_write_ProviderFile}\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_MEMORY_WRITE_API\\(([A-Za-z0-9_]+)\\)" "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_memory_write_dir}/provider.def" CONTENT "${_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsMemoryWriteCases.def")
set(_memory_write_outputs)
foreach(_arch X64 AArch64)
  set(_dir "${_memory_write_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(OUTPUT "${_dir}/${_memory_write_ProgramFile}" "${_dir}/program.obj"
      "${_dir}/provider.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_memory_write_${_arch}Machine}"
      "/def:${_memory_write_dir}/provider.def" "/out:${_dir}/provider.lib"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_memory_write_${_arch}Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -O1 -Wall -Wextra -Werror -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_memory_write_SourceFile}" -o "${_dir}/program.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib "/entry:${_memory_write_ProgramEntry}"
      /subsystem:console "/machine:${_memory_write_${_arch}Machine}"
      "/base:${_memory_write_ProgramBase}" /timestamp:0
      "${_dir}/program.obj" "${_dir}/provider.lib" "/out:${_dir}/${_memory_write_ProgramFile}"
    DEPENDS fixtures/windows_memory_write.c fixtures/WindowsMemoryWriteCases.def
      "${_memory_write_dir}/provider.def" VERBATIM)
  list(APPEND _memory_write_outputs "${_dir}/${_memory_write_ProgramFile}")
endforeach()
add_custom_target(NeverDWindowsMemoryWriteFixtures DEPENDS ${_memory_write_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsMemoryWriteFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_MEMORY_WRITE_FIXTURE_DIR="${_memory_write_dir}")
