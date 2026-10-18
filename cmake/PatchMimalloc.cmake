# Backport the macOS malloc-zone fork callback from mimalloc v3.5.3,
# src/prim/osx/alloc-override-zone.c. Copyright (c) 2018-2026 Microsoft Research,
# Daan Leijen; MIT license preserved in LICENSES/mimalloc/LICENSE.
# Invoked by FetchContent only for its downloaded, disposable source tree.
set(_file "${NEVERD_MIMALLOC_SOURCE_DIR}/src/prim/osx/alloc-override-zone.c")
file(READ "${_file}" _source)
if(_source MATCHES "\\.reinit_lock = &intro_reinit_lock")
  return()
endif()
set(_table "static malloc_introspection_t mi_introspect = {")
set(_entry "  .zone_locked = &intro_zone_locked,\n#endif")
string(FIND "${_source}" "${_table}" _table_offset)
string(FIND "${_source}" "${_entry}" _entry_offset)
if(_table_offset EQUAL -1 OR _entry_offset EQUAL -1)
  message(FATAL_ERROR "Unexpected mimalloc source; cannot apply macOS fork fix")
endif()
string(REPLACE "${_table}" [=[// NeverD 2026-10-18: backport the v3.5.3 macOS fork callback.
// macOS calls reinit_lock without a NULL check for zone version >= 9.
static void intro_reinit_lock(malloc_zone_t* zone) {
  MI_UNUSED(zone);
}

static malloc_introspection_t mi_introspect = {]=] _source "${_source}")
string(REPLACE "${_entry}" [=[  .zone_locked = &intro_zone_locked,
#endif
#if defined(MAC_OS_X_VERSION_10_12) && (MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_12) && !defined(__ppc__)
  .reinit_lock = &intro_reinit_lock,
#endif]=] _source "${_source}")
file(WRITE "${_file}" "${_source}")
