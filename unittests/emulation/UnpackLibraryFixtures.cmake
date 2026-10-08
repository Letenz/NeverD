if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
set(_library_cases "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/UnpackLibraryCases.def")
set(_library_dir "${CMAKE_CURRENT_BINARY_DIR}/unpack-library-fixtures")
file(MAKE_DIRECTORY "${_library_dir}")
file(STRINGS "${_library_cases}" _apis REGEX "^NEVERD_LIBRARY_API\\(")
set(_definition "LIBRARY kernel32.dll\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_LIBRARY_API\\(([A-Za-z0-9_]+)\\)"
    "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_library_dir}/kernel.def" CONTENT "${_definition}" @ONLY)
file(STRINGS "${_library_cases}" _exports REGEX "^NEVERD_LIBRARY_EXPORT\\(")
set(_definition "LIBRARY input.dll\nEXPORTS\n")
foreach(_export IN LISTS _exports)
  string(REGEX REPLACE "^NEVERD_LIBRARY_EXPORT\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)"
    "  \\1 \\2\n" _line "${_export}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_library_dir}/input.def" CONTENT "${_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_library_cases}")
set(_library_outputs)
foreach(_arch X64 AArch64)
  if(_arch STREQUAL "X64")
    set(_target x86_64-pc-windows-msvc)
    set(_machine x64)
  else()
    set(_target aarch64-pc-windows-msvc)
    set(_machine arm64)
  endif()
  set(_dir "${_library_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(OUTPUT "${_dir}/kernel.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_machine}"
      "/def:${_library_dir}/kernel.def" "/out:${_dir}/kernel.lib"
    DEPENDS "${_library_dir}/kernel.def" VERBATIM)
  foreach(_role dependency input host)
    set(_define)
    set(_link /dll /entry:dllEntry /base:0x180000000)
    set(_imports "${_dir}/kernel.lib")
    set(_extra)
    if(_role STREQUAL "dependency")
      set(_define -DNEVERD_LIBRARY_DEPENDENCY)
      list(APPEND _extra "${_dir}/dependency.lib")
    elseif(_role STREQUAL "host")
      set(_define -DNEVERD_LIBRARY_HOST)
      set(_link /entry:hostEntry /base:0x140000000)
    else()
      list(APPEND _link "/def:${_library_dir}/input.def" /include:_tls_used
        /section:.body,ERW)
      list(APPEND _imports "${_dir}/dependency.lib")
    endif()
    if(_role STREQUAL "host")
      set(_extension exe)
    else()
      set(_extension dll)
    endif()
    add_custom_command(OUTPUT "${_dir}/${_role}.${_extension}"
        "${_dir}/${_role}.obj" ${_extra}
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_target}"
        -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
        -fno-vectorize -fno-slp-vectorize -O1 -c ${_define}
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unpack_library.c"
        -o "${_dir}/${_role}.obj"
      COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /subsystem:console
        "/machine:${_machine}" /timestamp:0 ${_link}
        "${_dir}/${_role}.obj" ${_imports}
        "/out:${_dir}/${_role}.${_extension}"
      DEPENDS fixtures/unpack_library.c "${_library_cases}"
        "${_library_dir}/input.def" ${_imports} VERBATIM)
    list(APPEND _library_outputs "${_dir}/${_role}.${_extension}")
  endforeach()
endforeach()
add_custom_target(NeverDUnpackLibraryFixtures DEPENDS ${_library_outputs})
foreach(_test NeverDUnpackExecutionTests NeverDUnpackPublicTests)
  if(TARGET ${_test})
    add_dependencies(${_test} NeverDUnpackLibraryFixtures)
    target_compile_definitions(${_test} PRIVATE
      NEVERD_UNPACK_LIBRARY_FIXTURE_DIR="${_library_dir}")
  endif()
endforeach()
