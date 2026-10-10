//===- windows_lifetime_cleanup.c - Module released by an exit callback --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsLifetimeFixture.h"
__declspec(dllexport) DWORD CleanupIndex(void) { return _tls_index; }
static void tls(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Image != 0);
  checkTLS(Reason);
  trace(CleanupRole, TLSKind, Reason, Reserved);
}
int dllEntry(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Image != 0);
  trace(CleanupRole, DLLKind, Reason, Reserved);
  return 1;
}
