cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDMCP LANGUAGES C CXX)
  include(CTest)
endif()

if(NOT TARGET neverd_worker_engine)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../neverd-worker" worker)
endif()
find_package(Threads REQUIRED)

# The protocol, tools and result shapes; tests run them without an engine.
# This directory is on the include path, so no header here may share a
# standard header's name: case-insensitive file systems (macOS, Windows)
# would resolve <stdio.h> to a Stdio.h.
add_library(neverd_mcp_protocol STATIC Backend.cpp Reading.cpp Server.cpp StdioTransport.cpp Tools.cpp)
target_compile_features(neverd_mcp_protocol PUBLIC cxx_std_20)
target_include_directories(neverd_mcp_protocol PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(neverd_mcp_protocol PUBLIC neverd_tool_json Threads::Threads)
add_executable(neverd-mcp Main.cpp EngineBackend.cpp)
target_link_libraries(neverd-mcp PRIVATE neverd_mcp_protocol neverd_worker_engine)
if(MINGW)
  target_link_options(neverd-mcp PRIVATE -municode)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDWorkerAllocator.cmake")
neverd_worker_allocator(neverd-mcp)
set_target_properties(neverd-mcp PROPERTIES
  INSTALL_RPATH "$ORIGIN/../lib;@loader_path/../lib")
install(TARGETS neverd-mcp RUNTIME DESTINATION bin)
neverd_stage_json_notice(neverd-mcp)

if(BUILD_TESTING OR NEVERD_BUILD_TESTS)
  add_executable(neverd-mcp-tests tests/ProtocolTests.cpp tests/ReadingTests.cpp tests/AttachTests.cpp tests/Main.cpp)
  target_link_libraries(neverd-mcp-tests PRIVATE neverd_mcp_protocol)
  add_test(NAME NeverDMCP COMMAND neverd-mcp-tests)
  set_tests_properties(NeverDMCP PROPERTIES TIMEOUT 60 LABELS "mcp")
  # The process test reads real decompilation: a build against a mock
  # engine, such as the workbench's CI fixture, leaves it out.
  if(NOT DEFINED NEVERD_WORKER_REAL_ENGINE_TESTS OR NEVERD_WORKER_REAL_ENGINE_TESTS)
    add_executable(neverd-mcp-process-tests tests/ProcessTests.cpp)
    target_link_libraries(neverd-mcp-process-tests PRIVATE neverd_tool_json)
    add_dependencies(neverd-mcp-process-tests neverd-mcp)
    add_test(NAME NeverDMCPProcess COMMAND neverd-mcp-process-tests "$<TARGET_FILE:neverd-mcp>")
    set_tests_properties(NeverDMCPProcess PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "mcp")
  endif()
endif()
