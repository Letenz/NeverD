include_guard(GLOBAL)

# Prefer the platform's compression library. Windows source builds can use
# the pinned static fallback without a separately installed package manager.
find_package(ZLIB QUIET)
if(TARGET ZLIB::ZLIB)
  set_property(TARGET ZLIB::ZLIB PROPERTY IMPORTED_GLOBAL TRUE)
endif()
if(NOT TARGET ZLIB::ZLIB)
  include(FetchContent)
  function(neverd_fetch_zlib)
    if(POLICY CMP0135)
      cmake_policy(SET CMP0135 NEW)
    endif()
    set(ZLIB_BUILD_TESTING OFF)
    set(ZLIB_BUILD_SHARED OFF)
    set(ZLIB_BUILD_STATIC ON)
    set(ZLIB_INSTALL OFF)
    # Both maintainer-hosted archives have the same pinned bytes. Prefer the
    # release asset so CI need not rely on the fossils endpoint being available.
    FetchContent_Declare(neverd_mobile_zlib
      URL
        https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
        https://zlib.net/fossils/zlib-1.3.2.tar.gz
      URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
      TIMEOUT 180)
    FetchContent_MakeAvailable(neverd_mobile_zlib)
  endfunction()
  neverd_fetch_zlib()
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
