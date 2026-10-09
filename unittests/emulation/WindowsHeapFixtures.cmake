if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsHeapCases.def"
  _settings REGEX "^NEVERD_HEAP_(BUILD|TEXT)\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_HEAP_(BUILD|TEXT)\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_heap_${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endif()
endforeach()
set(_heap_dir "${CMAKE_CURRENT_BINARY_DIR}/${_heap_OutputDirectory}")
file(MAKE_DIRECTORY "${_heap_dir}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsHeapCases.def"
  _apis REGEX "^NEVERD_HEAP_API")
set(_definition "LIBRARY ${_heap_ProviderFile}\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_HEAP_API\\(([A-Za-z0-9_]+)\\)" "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_heap_dir}/provider.def" CONTENT "${_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsHeapCases.def")
set(_heap_outputs)
foreach(_arch X64 AArch64)
  set(_dir "${_heap_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(OUTPUT "${_dir}/${_heap_ProgramFile}" "${_dir}/program.obj"
      "${_dir}/provider.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_heap_${_arch}Machine}"
      "/def:${_heap_dir}/provider.def" "/out:${_dir}/provider.lib"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_heap_${_arch}Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_heap.c" -o "${_dir}/program.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib "/entry:${_heap_ProgramEntry}"
      /subsystem:console "/machine:${_heap_${_arch}Machine}"
      "/base:${_heap_ProgramBase}" /timestamp:0
      "${_dir}/program.obj" "${_dir}/provider.lib" "/out:${_dir}/${_heap_ProgramFile}"
    DEPENDS fixtures/windows_heap.c fixtures/WindowsPrivateHeapFixture.inc fixtures/WindowsHeapCases.def
      "${_heap_dir}/provider.def" VERBATIM)
  list(APPEND _heap_outputs "${_dir}/${_heap_ProgramFile}")
endforeach()
add_custom_target(NeverDWindowsHeapFixtures DEPENDS ${_heap_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsHeapFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_HEAP_FIXTURE_DIR="${_heap_dir}")
