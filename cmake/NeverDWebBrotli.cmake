# Optional pinned native codec. Never fetch or execute a decoder tool.
function(neverd_add_web_brotli)
  set(NEVERD_WEB_BROTLI_ROOT "" CACHE PATH "Native static Brotli SDK prefix")
  if(WIN32 AND NEVERD_WEB_BROTLI_ROOT)
    find_path(NEVERD_BROTLI_INCLUDE_DIR brotli/decode.h
      PATHS "${NEVERD_WEB_BROTLI_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(NEVERD_BROTLI_DEC_STATIC NAMES brotlidec-static brotlidec
      PATHS "${NEVERD_WEB_BROTLI_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    find_library(NEVERD_BROTLI_COMMON_STATIC NAMES brotlicommon-static brotlicommon
      PATHS "${NEVERD_WEB_BROTLI_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_include_directories(NeverDWeb PRIVATE "${NEVERD_BROTLI_INCLUDE_DIR}")
    target_link_libraries(NeverDWeb PRIVATE
      ${NEVERD_BROTLI_DEC_STATIC} ${NEVERD_BROTLI_COMMON_STATIC})
    set_property(SOURCE compression/Brotli.cpp APPEND PROPERTY
      COMPILE_DEFINITIONS NEVERD_WEB_BROTLI=1)
    # The decoder's authoritative runtime version check still requires 1.2.0.
    set(_neverd_windows_brotli ON)
  endif()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(NEVERD_BROTLI QUIET libbrotlidec=1.2.0 libbrotlicommon=1.2.0)
  endif()
  if(PkgConfig_FOUND AND NEVERD_BROTLI_FOUND)
    find_library(NEVERD_BROTLI_DEC_STATIC NAMES libbrotlidec.a
      PATHS ${NEVERD_BROTLI_LIBRARY_DIRS} NO_DEFAULT_PATH)
    find_library(NEVERD_BROTLI_COMMON_STATIC NAMES libbrotlicommon.a
      PATHS ${NEVERD_BROTLI_LIBRARY_DIRS} NO_DEFAULT_PATH)
  endif()
  if(_neverd_windows_brotli)
    message(STATUS "Web Brotli: using the selected Windows static SDK")
  elseif(PkgConfig_FOUND AND NEVERD_BROTLI_FOUND AND
     EXISTS "${NEVERD_BROTLI_DEC_STATIC}" AND EXISTS "${NEVERD_BROTLI_COMMON_STATIC}")
    target_include_directories(NeverDWeb PRIVATE ${NEVERD_BROTLI_INCLUDE_DIRS})
    target_link_libraries(NeverDWeb PRIVATE
      ${NEVERD_BROTLI_DEC_STATIC} ${NEVERD_BROTLI_COMMON_STATIC})
    set_property(SOURCE compression/Brotli.cpp APPEND PROPERTY
      COMPILE_DEFINITIONS NEVERD_WEB_BROTLI=1)
    message(STATUS "Web Brotli: using native static SDK 1.2.0")
  else()
    message(STATUS "Tauri Brotli profile unavailable: static Brotli SDK 1.2.0 required")
  endif()
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/brotli/"
    DESTINATION share/neverd/licenses/brotli)
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/phf/"
    DESTINATION share/neverd/licenses/phf)
endfunction()
