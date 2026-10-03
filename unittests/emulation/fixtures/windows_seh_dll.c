//===- windows_seh_dll.c - Original cross-image C exception callbacks ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef void (*TraceCall)(U32);
typedef int (*FilterCall)(void *);
#define NEVERD_USER_SEH_VALUE(Name, Value) enum { Name = Value };
#include "WindowsSEHCases.def"
__declspec(dllimport) void RaiseException(U32, U32, U32, const U64 *);
unsigned long _exception_code(void);
void *_exception_info(void);
int _abnormal_termination(void);
__declspec(dllexport) void RaiseFromDLL(TraceCall Trace, FilterCall Filter) {
  __try {
    RaiseException(SoftwareCode, 0, 0, 0);
  } __finally {
    Trace(_abnormal_termination() ? FinallyTrace : 0);
  }
}
__declspec(dllexport) void CatchInDLL(TraceCall Trace, FilterCall Filter) {
  __try {
    RaiseException(SoftwareCode, 0, 0, 0);
  } __except (Filter(_exception_info())) {
    Trace(_exception_code() == SoftwareCode ? HandlerTrace : 0);
  }
}
