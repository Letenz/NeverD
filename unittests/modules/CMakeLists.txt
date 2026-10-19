add_neverd_unittest(NeverDModuleTests
  ModuleSetTests.cpp
  LINK_COMPONENTS Modules)

find_program(NEVERD_MODULE_LLD_LINK NAMES lld-link
  HINTS "${LLVM_TOOLS_BINARY_DIR}")
if(NEVERD_TEST_CLANG_EXECUTABLE AND NEVERD_MODULE_LLD_LINK)
  set(_module_fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  set(_module_outputs)
  foreach(_variant first second)
    if(_variant STREQUAL "first")
      set(_value 42)
    else()
      set(_value 73)
    endif()
    set(_base "${_module_fixtures}/${_variant}")
    add_custom_command(
      OUTPUT "${_base}.sys" "${_base}.pdb"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_module_fixtures}"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
        --target=x86_64-pc-windows-msvc -O1 -g -gcodeview -ffreestanding
        -fno-stack-protector -DMODULE_VALUE=${_value}
        -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/module_input.c" -o "${_base}.obj"
      COMMAND "${NEVERD_MODULE_LLD_LINK}" /machine:x64 /driver /subsystem:native
        /entry:module_probe /nodefaultlib /debug "/pdb:${_base}.pdb"
        "/out:${_base}.sys" "${_base}.obj"
      DEPENDS fixtures/module_input.c
      VERBATIM)
    list(APPEND _module_outputs "${_base}.sys" "${_base}.pdb")
  endforeach()
  add_custom_target(NeverDModuleFixtures DEPENDS ${_module_outputs})
  add_dependencies(NeverDModuleTests NeverDModuleFixtures)
  target_compile_definitions(NeverDModuleTests PRIVATE
    "NEVERD_MODULE_FIXTURES=\"${_module_fixtures}\"")
endif()

add_neverd_unittest(NeverDExecutionCAPIDisabledTests
  ../ExecutionCAPIDisabledTests.cpp
  ${CMAKE_SOURCE_DIR}/lib/sdk/capi/NeverDCAPIExecution.cpp
  ${CMAKE_SOURCE_DIR}/lib/sdk/capi/ResourceHandles.cpp
  LINK_LIBS LLVMSupport)
target_include_directories(NeverDExecutionCAPIDisabledTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
target_compile_definitions(NeverDExecutionCAPIDisabledTests PRIVATE NEVERD_EXPORTS)

if(TARGET neverd_shared)
  if(TARGET NeverDEmulationWorkload)
    add_neverd_unittest(NeverDExecutionCAPITests
      ../ExecutionCAPITests.cpp
      ../ExecutionCAPISmoke.c
      TIMEOUT 30
      LINK_LIBS neverd_shared)
    target_include_directories(NeverDExecutionCAPITests PRIVATE
      ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
    if(WIN32)
      target_link_libraries(NeverDExecutionCAPITests PRIVATE LLVMSupport)
    endif()
  else()
    add_neverd_unittest(NeverDExecutionSDKDisabledTests
      ../ExecutionSDKDisabledTests.cpp
      ../ModuleCAPISmoke.c
      LINK_LIBS neverd_shared)
    target_include_directories(NeverDExecutionSDKDisabledTests PRIVATE
      ${CMAKE_SOURCE_DIR}/include)
  endif()
  add_neverd_unittest(NeverDModuleCAPITests
    ../ModuleCAPITests.cpp
    ../ModuleCAPISmoke.c
    LINK_LIBS neverd_shared)
  target_include_directories(NeverDModuleCAPITests PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
  if(WIN32)
    target_link_libraries(NeverDModuleCAPITests PRIVATE LLVMSupport)
  endif()
  if(TARGET NeverDModuleFixtures)
    add_dependencies(NeverDModuleCAPITests NeverDModuleFixtures)
    target_compile_definitions(NeverDModuleCAPITests PRIVATE
      "NEVERD_MODULE_CAPI_FIXTURES=\"${_module_fixtures}\"")
  endif()
  if(TARGET neverd)
    if(TARGET NeverDEmulationWorkload)
      add_test(NAME NeverDExecutionCLITests
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/scripts/tests/test_execution_cli.py"
          --neverd "$<TARGET_FILE:neverd>"
          --library "$<TARGET_FILE:neverd_shared>")
      set_tests_properties(NeverDExecutionCLITests PROPERTIES
        LABELS "NeverDExecutionCLITests;Execution;CLI"
        TIMEOUT 90)
    else()
      add_test(NAME NeverDExecutionSDKDisabledCLITests
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/scripts/tests/test_execution_disabled_cli.py"
          --neverd "$<TARGET_FILE:neverd>")
      set_tests_properties(NeverDExecutionSDKDisabledCLITests PROPERTIES
        LABELS "NeverDExecutionSDKDisabledCLITests;Execution;CLI"
        TIMEOUT 45)
    endif()
    add_test(NAME NeverDModuleCLITests
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/tests/test_module_cli.py"
        --neverd "$<TARGET_FILE:neverd>"
        --library "$<TARGET_FILE:neverd_shared>")
    set_tests_properties(NeverDModuleCLITests PROPERTIES
      LABELS "NeverDModuleCLITests;Modules;CLI"
      TIMEOUT 60)
  endif()
endif()
