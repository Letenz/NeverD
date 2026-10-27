# Process allocator policy for NeverD-owned executables. Keep the allocator
# out of component archives and libneverd: SDK hosts own their process heap.
# Inspired by NeverC's neverc/cmake/modules/AddMimalloc.cmake at
# 52e83efb3e3902c97130b9e3c83611080fa18ac4 (NeverC contributors, AGPL-3.0-only).
# Source: https://github.com/NeverSight/NeverC/blob/52e83efb3e3902c97130b9e3c83611080fa18ac4/neverc/cmake/modules/AddMimalloc.cmake
# See THIRD_PARTY_NOTICES.md for the integration changes and license notices.
# Uses upstream's object override on Unix and CRT DLL redirection on Windows.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/NeverDForks.cmake")

function(neverd_check_mimalloc_crt runtime)
  if(NOT "${runtime}" STREQUAL "" AND NOT "${runtime}" MATCHES "DLL$")
    message(FATAL_ERROR
      "Windows mimalloc requires the dynamic CRT (/MD or /MDd), but "
      "MSVC_RUNTIME_LIBRARY is '${runtime}'. Use a DLL runtime or "
      "-DNEVERD_ENABLE_MIMALLOC=OFF.")
  endif()
endfunction()

function(neverd_link_mimalloc target)
  if(NOT NEVERD_MIMALLOC_ENABLED)
    return()
  endif()
  get_target_property(_type ${target} TYPE)
  if(NOT _type STREQUAL "EXECUTABLE")
    message(FATAL_ERROR "mimalloc must only be linked into NeverD executables")
  endif()
  if(WIN32)
    get_target_property(_runtime ${target} MSVC_RUNTIME_LIBRARY)
    if(_runtime)
      neverd_check_mimalloc_crt("${_runtime}")
    endif()
    # Load mimalloc before the SDK/plugin DLL initializers. Prepend even when
    # the caller has already linked other libraries, and retain the import
    # when the executable itself makes no allocation calls.
    get_target_property(_libraries ${target} LINK_LIBRARIES)
    if(NOT _libraries)
      set(_libraries "")
    endif()
    set_property(TARGET ${target} PROPERTY LINK_LIBRARIES mimalloc ${_libraries})
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
      target_link_options(${target} PRIVATE /INCLUDE:_mi_version)
    else()
      target_link_options(${target} PRIVATE /INCLUDE:mi_version)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:mimalloc>"
        "$<TARGET_PROPERTY:mimalloc,NEVERD_REDIRECT_DLL>"
        "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
    get_property(_installed GLOBAL PROPERTY NEVERD_MIMALLOC_RUNTIME_INSTALL)
    if(NOT _installed)
      # Register only when an owned executable uses the allocator. SDK-only
      # builds must not try to install an unbuilt, excluded mimalloc target.
      install(FILES "$<TARGET_FILE:mimalloc>"
        "$<TARGET_PROPERTY:mimalloc,NEVERD_REDIRECT_DLL>" DESTINATION bin)
      set_property(GLOBAL PROPERTY NEVERD_MIMALLOC_RUNTIME_INSTALL ON)
    endif()
  else()
    # Direct object linkage retains the override/initialization code even when
    # the executable only calls into libneverd. An archive can be discarded.
    target_link_libraries(${target} PRIVATE mimalloc-obj)
  endif()
  get_property(_notices GLOBAL PROPERTY NEVERD_FORK_NOTICES_neverd_mimalloc)
  # Executables in the shared runtime directory take the notices from one
  # target: a copy after each link raced with the others' copies into the
  # same directory when they linked in parallel ("Error copying directory").
  # An executable with a directory of its own copies them there itself.
  get_target_property(_runtime_dir ${target} RUNTIME_OUTPUT_DIRECTORY)
  if(_runtime_dir STREQUAL "${CMAKE_BINARY_DIR}/bin")
    if(NOT TARGET neverd-mimalloc-notices)
      set(_notices_dir "${CMAKE_BINARY_DIR}/bin")
      get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
      if(_multi_config)
        string(APPEND _notices_dir "/$<CONFIG>")
      endif()
      add_custom_target(neverd-mimalloc-notices
        COMMAND ${CMAKE_COMMAND} -E make_directory
          "${_notices_dir}/licenses/mimalloc"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${_notices}"
          "${_notices_dir}/licenses/mimalloc"
        COMMENT "Copying the mimalloc notices next to NeverD executables"
        VERBATIM)
    endif()
    add_dependencies(${target} neverd-mimalloc-notices)
  else()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${target}>/licenses/mimalloc"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${_notices}"
        "$<TARGET_FILE_DIR:${target}>/licenses/mimalloc"
      VERBATIM)
  endif()
endfunction()

set(_neverd_mimalloc_default OFF)
if(CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$" OR (WIN32 AND MSVC))
  set(_neverd_mimalloc_default ON)
endif()
option(NEVERD_ENABLE_MIMALLOC
  "Use mimalloc in NeverD executables (Linux, macOS and Windows MSVC ABI)"
  ${_neverd_mimalloc_default})
set(NEVERD_MIMALLOC_ENABLED OFF)
if(NOT NEVERD_ENABLE_MIMALLOC)
  message(STATUS "NeverD allocator: system")
  return()
endif()

# Inspect every configured build type, including custom multi-config names.
# Do not FORCE the user's cache option OFF: removing sanitizer flags should
# restore the requested allocator on the next configure.
set(_neverd_allocator_flags "${LLVM_USE_SANITIZER}")
set(_neverd_allocator_configs Debug Release RelWithDebInfo MinSizeRel
  ${CMAKE_BUILD_TYPE} ${CMAKE_CONFIGURATION_TYPES})
foreach(_kind C CXX EXE_LINKER SHARED_LINKER MODULE_LINKER)
  string(APPEND _neverd_allocator_flags " ${CMAKE_${_kind}_FLAGS}")
  foreach(_config IN LISTS _neverd_allocator_configs)
    string(TOUPPER "${_config}" _config_upper)
    string(APPEND _neverd_allocator_flags
      " ${CMAKE_${_kind}_FLAGS_${_config_upper}}")
  endforeach()
endforeach()
string(TOLOWER "${_neverd_allocator_flags}" _neverd_allocator_flags)
# Match LLVM_USE_SANITIZER names as well as -fsanitize=undefined,address
# and MSVC's /fsanitize=address. UBSan alone does not intercept allocation.
string(TOLOWER "${LLVM_USE_SANITIZER}" _neverd_llvm_sanitizer)
if(_neverd_llvm_sanitizer MATCHES "address|thread|memory|leak" OR
   _neverd_allocator_flags MATCHES
     "[-/]fsanitize=[^ \t;]*(address|thread|memory|leak)")
  message(STATUS "NeverD allocator: system (allocator-intercepting sanitizer)")
  return()
endif()
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux|Windows)$")
  message(FATAL_ERROR
    "NEVERD_ENABLE_MIMALLOC supports Linux, macOS and Windows only. "
    "Use -DNEVERD_ENABLE_MIMALLOC=OFF on this platform.")
endif()
if(WIN32)
  if(NOT MSVC)
    message(FATAL_ERROR
      "Windows mimalloc requires an MSVC ABI toolchain (MSVC or clang-cl) "
      "with the dynamic CRT. Use -DNEVERD_ENABLE_MIMALLOC=OFF otherwise.")
  endif()
  neverd_check_mimalloc_crt("${CMAKE_MSVC_RUNTIME_LIBRARY}")
  if(_neverd_allocator_flags MATCHES "(^|[ \t;])[-/]mtd?([ \t;]|$)")
    message(FATAL_ERROR
      "Windows mimalloc requires the dynamic CRT (/MD or /MDd); remove "
      "the /MT or /MTd flags, or set -DNEVERD_ENABLE_MIMALLOC=OFF.")
  endif()
endif()

function(neverd_fetch_mimalloc)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/NeverDForks.cmake")
  neverd_populate_fork(neverd_mimalloc)
  if(APPLE)
    # Caller-owned fixtures must retain the mainline macOS fork callbacks.
    file(READ "${neverd_mimalloc_SOURCE_DIR}/src/prim/osx/alloc-override-zone.c"
      _zone_source)
    if(NOT _zone_source MATCHES "\\.reinit_lock = &intro_reinit_lock")
      message(FATAL_ERROR
        "The supplied mimalloc source needs the macOS fork callbacks. Unset "
        "FETCHCONTENT_SOURCE_DIR_NEVERD_MIMALLOC to use the NeverSight fork.")
    endif()
  endif()
  set(MI_BUILD_STATIC OFF)
  if(WIN32)
    set(MI_BUILD_SHARED ON)
    set(MI_BUILD_OBJECT OFF)
    set(MI_WIN_REDIRECT ON)
  else()
    set(MI_BUILD_SHARED OFF)
    set(MI_BUILD_OBJECT ON)
  endif()
  set(MI_BUILD_TESTS OFF)
  set(MI_OVERRIDE ON)
  set(MI_OSX_ZONE ON)
  set(MI_OSX_INTERPOSE ON)
  # Preserve NeverD's CPU baseline (upstream otherwise enables ARMv8.1).
  set(MI_NO_OPT_ARCH ON)
  # EXCLUDE_FROM_ALL also excludes upstream install rules, without replacing
  # CMake's install() command or breaking the parent installation.
  add_subdirectory("${neverd_mimalloc_SOURCE_DIR}"
    "${neverd_mimalloc_BINARY_DIR}" EXCLUDE_FROM_ALL)
  if(WIN32)
    get_directory_property(_redirect_suffix
      DIRECTORY "${neverd_mimalloc_SOURCE_DIR}" DEFINITION MIMALLOC_REDIRECT_SUFFIX)
    set(_redirect_dll
      "${neverd_mimalloc_SOURCE_DIR}/bin/mimalloc-redirect${_redirect_suffix}.dll")
    if(NOT EXISTS "${_redirect_dll}")
      message(FATAL_ERROR "No mimalloc redirection DLL for this Windows architecture")
    endif()
    target_compile_definitions(mimalloc INTERFACE MI_SHARED_LIB)
    set_target_properties(mimalloc PROPERTIES
      FOLDER "Third-Party" NEVERD_REDIRECT_DLL "${_redirect_dll}")
    # Upstream install rules are excluded. The executable helper deploys the
    # two runtime DLLs without installing allocator development files.
  else()
    # Upstream attaches its platform libraries to the archive/shared targets
    # only. The single-object consumer needs those dependencies too.
    get_directory_property(_platform_libraries
      DIRECTORY "${neverd_mimalloc_SOURCE_DIR}" DEFINITION mi_libraries)
    target_link_libraries(mimalloc-obj INTERFACE ${_platform_libraries})
    set_target_properties(mimalloc-obj PROPERTIES FOLDER "Third-Party")
  endif()
endfunction()

neverd_fetch_mimalloc()
set(NEVERD_MIMALLOC_ENABLED ON)
message(STATUS "NeverD allocator: mimalloc from NeverSight default branch (executables only)")
get_property(_neverd_mimalloc_notices GLOBAL PROPERTY NEVERD_FORK_NOTICES_neverd_mimalloc)
install(DIRECTORY "${_neverd_mimalloc_notices}/"
  DESTINATION share/neverd/licenses/mimalloc)
