//===- windows_lifetime_leaf.c - Original dependency TLS and entry --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsLifetimeFixture.h"
__declspec(dllexport) DWORD LeafIndex(void) {
  CHECK(ThreadPointer == &Sentinel && ThreadValue >= Seed);
  return _tls_index;
}
static void tls(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Image != 0);
  checkTLS(Reason);
  trace(LeafRole, TLSKind, Reason, Reserved);
  if (Reason == AttachReason && mode() == FaultMode)
    *(volatile DWORD *)(ULONG_PTR)FaultAddress = Seed;
  if (Reason == AttachReason && mode() == ExitLeafTLSMode)
    ExitProcess(ExitStatus);
}
int dllEntry(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Image != 0);
  trace(LeafRole, DLLKind, Reason, Reserved);
  ++ThreadValue;
  if (Reason == AttachReason) {
    if (mode() == ExitLeafEntryMode)
      ExitProcess(ExitStatus);
    return mode() != FailLeafMode;
  }
  return 0;
}
