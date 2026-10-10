//===- windows_lifetime_process.c - Original startup and exit oracle ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsLifetimeFixture.h"
__declspec(dllimport) DWORD LeafIndex(void);
__declspec(dllimport) DWORD MiddleIndex(void);
__declspec(dllimport) void PrepareFLSUnload(void);
static void tls(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Image != 0);
  checkTLS(Reason);
  trace(MainRole, TLSKind, Reason, Reserved);
  if (Reason == AttachReason) {
    CHECK(LeafIndex() != _tls_index && MiddleIndex() != _tls_index);
    if (mode() == ExitMainTLSMode)
      ExitProcess(ExitStatus);
  }
}
DWORD entry(void) {
  CHECK(ThreadValue == Seed + 1);
  CHECK(LeafIndex() != MiddleIndex());
  if (mode() == FLSUnloadMode)
    PrepareFLSUnload();
  trace(MainRole, EntryKind, 0, 0);
  if (mode() == ReturnMode)
    return ExitStatus;
  ExitProcess(ExitStatus);
  return FailureStatus;
}
