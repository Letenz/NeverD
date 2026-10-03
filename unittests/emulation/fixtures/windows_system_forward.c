//===- windows_system_forward.c - Original no-entry forwarder DLL --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Supply a real object: a definitions-only link can emit just an import
// library.
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllexport) void *SystemForwardMarker(void) {
  return GetProcessHeap();
}
