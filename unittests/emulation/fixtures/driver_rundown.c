//===- driver_rundown.c - Original run-down protection fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Drains accesses to a shared object with run-down protection the way a driver
/// guards teardown, without host or WDK dependencies. The acquire results are
/// printed; the FALSE after the wait is what sets run-down apart from a plain
/// reference count.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned long long U64;
typedef int NTSTATUS;
// x64 wdm.h EX_RUNDOWN_REF is a single pointer-sized Count word.
typedef struct {
  U64 Count;
} EX_RUNDOWN_REF;

__declspec(dllimport) U64 DbgPrint(const char *, ...);
__declspec(dllimport) void ExInitializeRundownProtection(EX_RUNDOWN_REF *);
__declspec(dllimport) void ExReInitializeRundownProtection(EX_RUNDOWN_REF *);
__declspec(dllimport) U8 ExAcquireRundownProtection(EX_RUNDOWN_REF *);
__declspec(dllimport) void ExReleaseRundownProtection(EX_RUNDOWN_REF *);
__declspec(dllimport) void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *);

static EX_RUNDOWN_REF Ref;

NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  ExInitializeRundownProtection(&Ref);
  U8 First = ExAcquireRundownProtection(&Ref);
  ExReleaseRundownProtection(&Ref);
  U8 Second = ExAcquireRundownProtection(&Ref);
  ExReleaseRundownProtection(&Ref);
  // Drain the object: with nothing outstanding the wait marks it run down.
  ExWaitForRundownProtectionRelease(&Ref);
  U8 AfterRundown = ExAcquireRundownProtection(&Ref);
  // Reinitialize for a new object and protect it again.
  ExReInitializeRundownProtection(&Ref);
  U8 Reused = ExAcquireRundownProtection(&Ref);
  ExReleaseRundownProtection(&Ref);
  DbgPrint("rundown %u %u %u %u", First, Second, AfterRundown, Reused);
  return 0;
}
