//===- windows_dynamic_middle.c - Original runtime dependency owner ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#define NEVERD_DYNAMIC_TLS
#include "WindowsDynamicFixture.h"
__declspec(dllimport) DWORD Leaf(void);
__declspec(dllexport) DWORD Probe(void) {
  CHECK(Leaf() >= Seed);
  return ThreadValue;
}
static void tls(void *Base, DWORD Reason, void *Reserved) {
  checkTLS(Reason);
  trace(MiddleRole, TLSKind, Reason, Reserved);
}
int dllEntry(void *Base, DWORD Reason, void *Reserved) {
  trace(MiddleRole, DLLKind, Reason, Reserved);
  if ((Reason == AttachReason && mode() == ExitAttachMode) ||
      (Reason == DetachReason && mode() == ExitDetachMode))
    ExitProcess(ExitStatus);
  if (Reason == AttachReason &&
      (mode() == NestedMode || mode() == NestedFailureMode)) {
    void *Top = LoadLibraryA(TopFile);
    CHECK(Top);
    if (mode() == NestedMode)
      observation(NestedTag, FreeLibrary(Top));
  }
  return !(Reason == AttachReason &&
           (mode() == FailedMiddleMode || mode() == NestedFailureMode));
}
