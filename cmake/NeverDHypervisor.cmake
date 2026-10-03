# Sign the process that calls Hypervisor.framework, not its shared library.
include_guard(GLOBAL)
set(NEVERD_HVF_SIGN_IDENTITY "-" CACHE STRING "Signing identity for HVF executables (default: ad-hoc)")
function(neverd_sign_hypervisor target)
  cmake_parse_arguments(PARSE_ARGV 1 _sign "IMPORTED_ENGINE" "" "")
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    return()
  endif()
  # Standalone consumers cannot inspect the imported engine's build flags.
  if(NOT _sign_IMPORTED_ENGINE AND
     (NOT NEVERD_EMULATION_BACKEND_HVF OR
      NOT (NEVERD_ENABLE_CPU_EMULATION OR NEVERD_ENABLE_DRIVER_EMULATION)))
    return()
  endif()
  find_program(NEVERD_CODESIGN codesign REQUIRED)
  set(_entitlements "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../resources/macos/neverd-hypervisor.entitlements")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${_entitlements}")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${NEVERD_CODESIGN}" --force --sign "${NEVERD_HVF_SIGN_IDENTITY}"
      --entitlements "${_entitlements}" "$<TARGET_FILE:${target}>"
    VERBATIM)
endfunction()
