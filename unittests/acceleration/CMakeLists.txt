cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDAccelerationTests LANGUAGES C CXX)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  include(CTest)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF)
  FetchContent_MakeAvailable(googletest)
  include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDMimalloc.cmake")
  if(DEFINED NEVERD_TEST_EXPECT_MIMALLOC)
    if(NEVERD_TEST_EXPECT_MIMALLOC AND NOT NEVERD_MIMALLOC_ENABLED)
      message(FATAL_ERROR "This configuration must enable mimalloc")
    elseif(NOT NEVERD_TEST_EXPECT_MIMALLOC AND NEVERD_MIMALLOC_ENABLED)
      message(FATAL_ERROR "This configuration must use the system allocator")
    endif()
  endif()
  if(MSVC)
    add_compile_options(/utf-8 /bigobj)
    # Match the production LLVM build: dependencies must tolerate Unicode APIs.
    add_compile_definitions(UNICODE _UNICODE)
  endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDPerformance.cmake")
neverd_add_text_acceleration()
neverd_add_task_scheduler()
if(NOT TARGET neverd_tool_json)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/common/transport" transport)
endif()

add_executable(NeverDAccelerationTests ByteScanTests.cpp SchedulerTests.cpp JsonTests.cpp AllocatorTests.cpp)
if(TARGET NeverDUnitTests)
  add_dependencies(NeverDUnitTests NeverDAccelerationTests)
endif()
target_compile_features(NeverDAccelerationTests PRIVATE cxx_std_20)
target_link_libraries(NeverDAccelerationTests PRIVATE
  neverd_text_acceleration neverd_task_scheduler neverd_tool_json GTest::gtest_main)
neverd_link_mimalloc(NeverDAccelerationTests)
target_compile_definitions(NeverDAccelerationTests PRIVATE
  NEVERD_USE_TBB=$<BOOL:${NEVERD_ENABLE_TBB}>
  NEVERD_TEST_MIMALLOC=$<BOOL:${NEVERD_MIMALLOC_ENABLED}>
  NEVERD_TEST_SIMDUTF=$<BOOL:${NEVERD_ENABLE_SIMDUTF}>
  NEVERD_TEST_HIGHWAY=$<BOOL:${NEVERD_ENABLE_HIGHWAY}>)
if(NEVERD_ENABLE_HIGHWAY)
  target_link_libraries(NeverDAccelerationTests PRIVATE hwy)
endif()
if(NEVERD_ENABLE_TBB)
  target_link_libraries(NeverDAccelerationTests PRIVATE TBB::tbb)
endif()
neverd_stage_task_runtime(NeverDAccelerationTests)
neverd_stage_performance_notices(NeverDAccelerationTests)
install(TARGETS NeverDAccelerationTests RUNTIME DESTINATION bin)
if(APPLE)
  set_property(TARGET NeverDAccelerationTests PROPERTY INSTALL_RPATH "@loader_path/../lib")
elseif(UNIX)
  set_property(TARGET NeverDAccelerationTests PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
endif()
include(GoogleTest)
gtest_discover_tests(NeverDAccelerationTests DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 45 LABELS NeverDAccelerationTests)

# A developer benchmark, not a CTest timing threshold. Reuses the actual text
# normalizer; it is available when the parent already provides LLVM Support.
if(TARGET LLVMSupport)
  add_executable(neverd-acceleration-bench EXCLUDE_FROM_ALL Benchmark.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/support/TextEncoding.cpp")
  target_compile_features(neverd-acceleration-bench PRIVATE cxx_std_20)
  target_include_directories(neverd-acceleration-bench PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(neverd-acceleration-bench PRIVATE neverd_text_acceleration
    neverd_task_scheduler neverd_tool_json LLVMSupport LLVMDemangle)
  neverd_link_mimalloc(neverd-acceleration-bench)
  neverd_stage_task_runtime(neverd-acceleration-bench)
endif()
