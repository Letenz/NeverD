//===- windows_dynamic_top.c - Original demand forwarder owner -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsDynamicFixture.h"
int dllEntry(void *Base, DWORD Reason, void *Reserved) {
  trace(TLSKind, DLLKind, Reason, Reserved);
  return 1;
}
