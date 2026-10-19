# Optional acceleration libraries. Every download is pinned; no allocator
# replacement is supplied by these dependencies.
include_guard(GLOBAL)
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

option(NEVERD_ENABLE_HIGHWAY "Use Highway for bounded byte scans" ON)
option(NEVERD_ENABLE_SIMDUTF "Use simdutf for valid UTF-8 fast paths" ON)
option(NEVERD_ENABLE_SIMDJSON "Use simdjson for transport admission" ON)
option(NEVERD_ENABLE_TBB "Use experimental oneTBB pipeline scheduling (no fork or SDK unload)" OFF)

function(neverd_add_task_scheduler)
  if(TARGET neverd_task_scheduler)
    return()
  endif()
  get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  add_library(neverd_task_scheduler STATIC "${_root}/lib/support/Parallel.cpp")
  target_compile_features(neverd_task_scheduler PUBLIC cxx_std_20)
  target_include_directories(neverd_task_scheduler PUBLIC "${_root}/include")
  set_target_properties(neverd_task_scheduler PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(NOT NEVERD_ENABLE_TBB)
    return()
  endif()
  FetchContent_Declare(neverd_tbb
    URL https://codeload.github.com/NeverSight/oneTBB/tar.gz/80dfa673b2d9df4edaa0d7ad2b6d28cc6ecae2e5
    URL_HASH SHA256=a0454a01f6516a094edaf0d3d264a2ba9e52492ce5912dae5d0264c2fb95f771
    SOURCE_SUBDIR neverd-no-automatic-subdirectory TIMEOUT 120)
  FetchContent_MakeAvailable(neverd_tbb)
  set(TBB_TEST OFF)
  set(TBB_EXAMPLES OFF)
  set(TBB_STRICT OFF)
  set(TBBMALLOC_BUILD OFF)
  set(TBBMALLOC_PROXY_BUILD OFF)
  set(TBB_INSTALL OFF)
  set(TBB_ENABLE_IPO OFF)
  set(TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH ON)
  set(BUILD_SHARED_LIBS ON)
  add_subdirectory("${neverd_tbb_SOURCE_DIR}"
    "${neverd_tbb_BINARY_DIR}" EXCLUDE_FROM_ALL)
  target_compile_definitions(neverd_task_scheduler PRIVATE NEVERD_USE_TBB=1)
  target_link_libraries(neverd_task_scheduler PRIVATE TBB::tbb)
  install(TARGETS tbb RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
  install(DIRECTORY "${_root}/LICENSES/oneTBB/" DESTINATION share/neverd/licenses/oneTBB)
endfunction()

function(neverd_stage_task_runtime Target)
  if(NEVERD_ENABLE_TBB AND WIN32 AND TARGET tbb)
    add_custom_command(TARGET ${Target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:tbb>"
        "$<TARGET_FILE_DIR:${Target}>" VERBATIM)
  endif()
endfunction()

function(neverd_stage_performance_notices Target)
  foreach(_pair "HIGHWAY;highway" "SIMDUTF;simdutf" "SIMDJSON;simdjson" "TBB;oneTBB")
    list(GET _pair 0 _option)
    list(GET _pair 1 _directory)
    if(NEVERD_ENABLE_${_option})
      add_custom_command(TARGET ${Target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../LICENSES/${_directory}"
          "$<TARGET_FILE_DIR:${Target}>/licenses/${_directory}" VERBATIM)
    endif()
  endforeach()
endfunction()

function(neverd_add_text_acceleration)
  if(TARGET neverd_text_acceleration)
    return()
  endif()
  get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  add_library(neverd_text_acceleration STATIC
    "${_root}/lib/support/ByteScan.cpp"
    "${_root}/lib/support/Unicode.cpp")
  target_compile_features(neverd_text_acceleration PUBLIC cxx_std_20)
  target_include_directories(neverd_text_acceleration PUBLIC "${_root}/include")
  set_target_properties(neverd_text_acceleration PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
  if(NEVERD_ENABLE_HIGHWAY)
    FetchContent_Declare(neverd_highway
      URL https://codeload.github.com/NeverSight/highway/tar.gz/2607d3b5b0113992fe84d3848859eae13b3b52c1
      URL_HASH SHA256=741d705781e0b3e406beda8f1f994fbae01321237ce8023a1ad90fbaf7940c25
      SOURCE_SUBDIR neverd-no-automatic-subdirectory TIMEOUT 120)
    FetchContent_MakeAvailable(neverd_highway)
    # Highway declares these with set(CACHE), rather than option(). Its older
    # policy baseline would otherwise discard ordinary scoped overrides.
    set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "Build Highway contrib" FORCE)
    set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "Build Highway examples" FORCE)
    set(HWY_ENABLE_TESTS OFF CACHE BOOL "Build Highway tests" FORCE)
    set(HWY_ENABLE_INSTALL OFF CACHE BOOL "Install Highway development files" FORCE)
    set(HWY_FORCE_STATIC_LIBS ON)
    add_subdirectory("${neverd_highway_SOURCE_DIR}"
      "${neverd_highway_BINARY_DIR}" EXCLUDE_FROM_ALL)
    set_target_properties(hwy PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(neverd_text_acceleration PRIVATE hwy)
    target_compile_definitions(neverd_text_acceleration PRIVATE NEVERD_USE_HIGHWAY=1)
    target_include_directories(neverd_text_acceleration PRIVATE "${_root}/lib/support")
    install(DIRECTORY "${_root}/LICENSES/highway/" DESTINATION share/neverd/licenses/highway)
  endif()
  if(NEVERD_ENABLE_SIMDUTF)
    FetchContent_Declare(neverd_simdutf
      URL https://codeload.github.com/NeverSight/simdutf/tar.gz/8d82299606e675b50de8dc16fd118aae4b65bfde
      URL_HASH SHA256=1570c58a689e5fd6a71a8fb661dbc33fdc6844a0a476a048a9e0dfde9ccc1ab8
      SOURCE_SUBDIR neverd-no-automatic-subdirectory TIMEOUT 120)
    FetchContent_MakeAvailable(neverd_simdutf)
    set(SIMDUTF_TESTS OFF)
    set(SIMDUTF_TOOLS OFF)
    set(SIMDUTF_BENCHMARKS OFF)
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${neverd_simdutf_SOURCE_DIR}"
      "${neverd_simdutf_BINARY_DIR}" EXCLUDE_FROM_ALL)
    set_target_properties(simdutf PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(neverd_text_acceleration PRIVATE simdutf)
    target_compile_definitions(neverd_text_acceleration PRIVATE NEVERD_USE_SIMDUTF=1)
    install(DIRECTORY "${_root}/LICENSES/simdutf/" DESTINATION share/neverd/licenses/simdutf)
  endif()
endfunction()

function(neverd_add_simdjson)
  if(NOT NEVERD_ENABLE_SIMDJSON OR TARGET simdjson)
    return()
  endif()
  FetchContent_Declare(neverd_simdjson
    URL https://codeload.github.com/NeverSight/simdjson/tar.gz/9c003534b1be3bc4f1322bfa7a770a7ffbf247d9
    URL_HASH SHA256=3d753ab15ce01a094066fff4c80719d1f03090c7ef140bec758dc3c0282c2a9f
    SOURCE_SUBDIR neverd-no-automatic-subdirectory TIMEOUT 120)
  FetchContent_MakeAvailable(neverd_simdjson)
  set(SIMDJSON_DEVELOPER_MODE OFF)
  set(BUILD_SHARED_LIBS OFF)
  add_subdirectory("${neverd_simdjson_SOURCE_DIR}"
    "${neverd_simdjson_BINARY_DIR}" EXCLUDE_FROM_ALL)
  set_target_properties(simdjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
  install(DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../LICENSES/simdjson/"
    DESTINATION share/neverd/licenses/simdjson)
endfunction()
