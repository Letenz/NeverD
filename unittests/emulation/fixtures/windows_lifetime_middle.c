//===- windows_lifetime_middle.c - Original dependent TLS and entry -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsLifetimeFixture.h"
__declspec(dllimport) DWORD LeafIndex(void);
__declspec(dllexport) DWORD MiddleIndex(void) {
  CHECK(ThreadPointer == &Sentinel && ThreadValue >= Seed);
  CHECK(LeafIndex() != _tls_index);
  return _tls_index;
}
static void tls(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Image != 0 && LeafIndex() != _tls_index);
  checkTLS(Reason);
  trace(MiddleRole, TLSKind, Reason, Reserved);
}
int dllEntry(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Image != 0);
  trace(MiddleRole, DLLKind, Reason, Reserved);
  if (Reason == AttachReason) {
    CHECK(LeafIndex() != _tls_index);
    ++ThreadValue;
    if (mode() == ExitMiddleEntryMode)
      ExitProcess(ExitStatus);
    return mode() != FailMiddleMode;
  }
  return 0;
}
