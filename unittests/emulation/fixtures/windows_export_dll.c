//===- windows_export_dll.c - Original forwarded DLL graph --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsExportFixture.h"
#if defined(EXPORT_LEAF)
DWORD Value;
DWORD Probe(void) { return Value; }
DWORD OrdinalProbe(void) { return Value + 1; }
#endif
int DllMain(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Reserved != 0);
  const char *Event;
#if defined(EXPORT_LEAF)
  Event = Reason ? LeafAttach : LeafDetach;
  if (Reason) {
    CHECK(lookup(Image, ProbeName) == (void *)&Probe);
    CHECK(lookup(Image, AliasName) == (void *)&Probe);
    Value = Seed;
  }
#elif defined(EXPORT_BRIDGE)
  Event = Reason ? BridgeAttach : BridgeDetach;
  if (Reason)
    CHECK(lookup(Image, DotName));
#else
  Event = Reason ? TopAttach : TopDetach;
  if (Reason)
    CHECK(lookup(Image, TopName));
#endif
  output(Event, 1);
  return 1;
}
