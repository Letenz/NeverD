//===- driver_identity.c - Original thread and process identity fixture ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compares the exported thread and process identity routines with their
/// kernel aliases and with the inline GS:[0x188] thread read, without host or
/// WDK dependencies.
///
//===----------------------------------------------------------------------===//

typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;

__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) void *PsGetCurrentProcess(void);
__declspec(dllimport) void *IoGetCurrentProcess(void);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) void *KeGetCurrentThread(void);
__declspec(dllimport) void *PsGetCurrentProcessId(void);
__declspec(dllimport) void *PsGetProcessId(void *);
__declspec(dllimport) void *PsGetCurrentThreadId(void);
__declspec(dllimport) void *PsGetThreadId(void *);

NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  // x64 wdm.h inlines KeGetCurrentThread as this read.
  U64 Inline;
  __asm__ volatile("movq %%gs:0x188, %0" : "=r"(Inline));
  void *Process = PsGetCurrentProcess();
  void *Thread = PsGetCurrentThread();
  void *ThreadId = PsGetCurrentThreadId();
  if (!Process || Process != IoGetCurrentProcess() || !Thread ||
      Thread != KeGetCurrentThread() || (U64)Thread != Inline ||
      PsGetProcessId(Process) != PsGetCurrentProcessId() || !ThreadId ||
      ((U64)ThreadId & 3) || PsGetThreadId(Thread) != ThreadId)
    return (NTSTATUS)0xc000000dU;
  DbgPrint("identity %u %u", (U32)(U64)PsGetProcessId(Process),
           (U32)(U64)ThreadId);
  return 0;
}
