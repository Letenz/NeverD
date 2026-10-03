//===- windows_exception_dll.c - Original loader exception callback ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
#define NEVERD_VEH_VALUE(Name, Value) enum { Name = Value };
#include "WindowsExceptionCases.def"
#undef NEVERD_VEH_VALUE
__declspec(dllimport) void RaiseException(U32, U32, U32, const U64 *);
int DllMain(void *Module, U32 Reason, void *Reserved) {
  if (Reason <= 1) {
    const U64 Argument = DataValue;
    RaiseException(Reason ? AttachCode : DetachCode, 0, 1, &Argument);
  }
  return 1;
}
