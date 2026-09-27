include(FetchContent)
# Keep the library and its generated headers at one reproducible revision.
FetchContent_Declare(neverd_z3
  GIT_REPOSITORY https://github.com/Z3Prover/z3.git
  GIT_TAG 54d30f26f72ce62f5dcb5a5258f632f84858714f # z3-4.13.3
  GIT_PROGRESS TRUE)

# This directory keeps dependency build settings out of NeverD's targets.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(Z3_BUILD_LIBZ3_SHARED OFF)
set(Z3_BUILD_LIBZ3_MSVC_STATIC OFF)
set(Z3_BUILD_EXECUTABLE OFF)
set(Z3_BUILD_TEST_EXECUTABLES OFF)
set(Z3_ENABLE_EXAMPLE_TARGETS OFF)
set(Z3_BUILD_PYTHON_BINDINGS OFF)
set(Z3_BUILD_DOTNET_BINDINGS OFF)
set(Z3_BUILD_JAVA_BINDINGS OFF)
set(Z3_BUILD_JULIA_BINDINGS OFF)
set(Z3_BUILD_DOCUMENTATION OFF)
set(Z3_INCLUDE_GIT_HASH OFF)
set(Z3_INCLUDE_GIT_DESCRIBE OFF)
FetchContent_MakeAvailable(neverd_z3)

add_library(NeverDZ3 INTERFACE IMPORTED GLOBAL)
set_target_properties(NeverDZ3 PROPERTIES
  INTERFACE_LINK_LIBRARIES libz3
  INTERFACE_INCLUDE_DIRECTORIES
    "${neverd_z3_SOURCE_DIR}/src/api;${neverd_z3_SOURCE_DIR}/src/api/c++;${neverd_z3_BINARY_DIR}/src/util")

if(CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF")
  # Bind the embedded solver locally when the host already loads another Z3.
  set_property(TARGET NeverDZ3 PROPERTY INTERFACE_LINK_OPTIONS
    "LINKER:--exclude-libs,$<TARGET_FILE_NAME:libz3>")
endif()
