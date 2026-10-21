//===- driver_process_peb.c - Original process environment queries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

typedef unsigned int U32;
typedef unsigned long long U64;
__declspec(dllimport) void *PsGetCurrentProcess(void);
__declspec(dllimport) void *PsGetProcessPeb(void *);

int DriverEntry(void *Driver, void *RegistryPath);
// Retain an actual DIR64 field and verify it after loading at either base.
static void *volatile EntryAddress = (void *)DriverEntry;

int DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  if (EntryAddress != (void *)DriverEntry)
    return (int)0xc000000dU;
  void *Process = PsGetCurrentProcess();
  void *First = PsGetProcessPeb(Process);
  void *Second = PsGetProcessPeb(Process);
  if (First != Second)
    return (int)0xc000000dU;
  // Independent negative callers: a returned numeric value is neither a
  // process token nor a declaration of readable storage.
  if ((U64)First == 1)
    return (int)(U64)PsGetProcessPeb(First);
  if (First)
    return *(volatile U32 *)First;
  return 0;
}
