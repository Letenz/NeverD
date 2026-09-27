# Disabled builds neither fetch nor discover Z3.
option(NEVERD_ENABLE_Z3 "Enable the Z3 bitvector solver backend" OFF)
set(NEVERD_Z3_PROVIDER "FETCH" CACHE STRING "Z3 dependency provider: FETCH or SYSTEM")
set_property(CACHE NEVERD_Z3_PROVIDER PROPERTY STRINGS FETCH SYSTEM)

if(NEVERD_ENABLE_Z3)
  if(NEVERD_Z3_PROVIDER STREQUAL "FETCH")
    add_subdirectory("${CMAKE_SOURCE_DIR}/cmake/z3"
      "${CMAKE_CURRENT_BINARY_DIR}/z3" EXCLUDE_FROM_ALL)
  elseif(NEVERD_Z3_PROVIDER STREQUAL "SYSTEM")
    set(Z3_ROOT "" CACHE PATH "Optional prefix containing Z3 headers and library")
    find_package(Z3 CONFIG QUIET HINTS "${Z3_ROOT}" "$ENV{Z3_ROOT}")
    if(TARGET Z3::libz3)
      set(NEVERD_Z3_TARGET Z3::libz3)
    elseif(TARGET z3::libz3)
      set(NEVERD_Z3_TARGET z3::libz3)
    elseif(TARGET libz3)
      set(NEVERD_Z3_TARGET libz3)
    else()
      find_path(NEVERD_Z3_INCLUDE_DIR NAMES z3++.h
        HINTS "${Z3_ROOT}" "$ENV{Z3_ROOT}" PATH_SUFFIXES include)
      find_library(NEVERD_Z3_LIBRARY NAMES z3 libz3
        HINTS "${Z3_ROOT}" "$ENV{Z3_ROOT}"
        PATH_SUFFIXES "lib/${CMAKE_LIBRARY_ARCHITECTURE}" lib lib64 bin)
      if(NOT NEVERD_Z3_INCLUDE_DIR OR NOT NEVERD_Z3_LIBRARY)
        message(FATAL_ERROR
          "NEVERD_Z3_PROVIDER=SYSTEM requires Z3 headers and library; install "
          "the development package, set Z3_ROOT, or use provider FETCH.")
      endif()
      add_library(NeverDZ3Library UNKNOWN IMPORTED GLOBAL)
      set_target_properties(NeverDZ3Library PROPERTIES
        IMPORTED_LOCATION "${NEVERD_Z3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NEVERD_Z3_INCLUDE_DIR}")
      set(NEVERD_Z3_TARGET NeverDZ3Library)
      mark_as_advanced(NEVERD_Z3_INCLUDE_DIR NEVERD_Z3_LIBRARY)
    endif()
    # Solver oracle tests live in a sibling directory.
    get_property(_neverd_z3_imports DIRECTORY PROPERTY IMPORTED_TARGETS)
    foreach(_neverd_z3_import IN LISTS _neverd_z3_imports)
      set_property(TARGET ${_neverd_z3_import} PROPERTY IMPORTED_GLOBAL TRUE)
    endforeach()
    add_library(NeverDZ3 INTERFACE IMPORTED GLOBAL)
    set_target_properties(NeverDZ3 PROPERTIES
      INTERFACE_LINK_LIBRARIES "${NEVERD_Z3_TARGET}")
  else()
    message(FATAL_ERROR "NEVERD_Z3_PROVIDER must be FETCH or SYSTEM")
  endif()
endif()
