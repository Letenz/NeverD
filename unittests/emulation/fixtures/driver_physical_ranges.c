//===- driver_physical_ranges.c - Original simulated RAM query fixture ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original simulated RAM query fixture.
///
//===----------------------------------------------------------------------===//
// This oracle describes the explicit model inventory, not native host RAM.
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef U64 *(*Query)(void);
__declspec(dllimport) U64 *MmGetPhysicalMemoryRanges(void);
__declspec(dllimport) void ExFreePool(void *);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void RtlInitUnicodeString(void *, const unsigned short *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(void *);
__declspec(dllimport) NTSTATUS IoCreateDevice(void *, U32, void *, U32, U32, U8,
                                              void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void IofCompleteRequest(void *, U8);
__declspec(dllimport) U32 DbgPrint(const char *, ...);

static void *Device;
static U64 *Retained;
static Query DynamicQuery;
NTSTATUS DriverEntry(U8 *, void *);
static void *volatile EntryAddress = (void *)DriverEntry;

static int valid(const U64 *P) {
  return P && P[0] == 0x1000000000ULL && P[1] == 0x4000000ULL && P[2] == 0 &&
         P[3] == 0;
}
static NTSTATUS Dispatch(void *Target, U8 *IRP) {
  U8 *Stack = *(U8 **)(IRP + 184);
  U64 Information = 0;
  NTSTATUS Status = Target == Device ? 0 : (NTSTATUS)0xc000000d;
  if (!Status && Stack[0] == 14) {
    U32 Code = *(U32 *)(Stack + 24);
    if (Code == 0x222004) {
      ExFreePool(Retained);
      // Deliberately invalid request: checked execution must stop here.
      **(U64 **)(IRP + 24) = *(volatile U64 *)Retained;
      Retained = 0;
      Information = 8;
    } else {
      U64 *Next = DynamicQuery();
      if (!valid(Next) || !valid(Retained) || Next == Retained)
        Status = (NTSTATUS)0xc000000d;
      if (!Status) {
        for (unsigned I = 0; I < 4; ++I)
          (*(U64 **)(IRP + 24))[I] = Next[I];
        Information = 32;
      }
      if (Next)
        ExFreePoolWithTag(Next, 0);
    }
  }
  *(NTSTATUS *)(IRP + 48) = Status;
  *(U64 *)(IRP + 56) = Information;
  IofCompleteRequest(IRP, 0);
  return Status;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (!valid(Retained))
    __builtin_trap();
  ExFreePool(Retained);
  Retained = 0;
  IoDeleteDevice(Device);
  DbgPrint("physical ranges unload complete");
}
NTSTATUS DriverEntry(U8 *Driver, void *RegistryPath) {
  (void)RegistryPath;
  if (EntryAddress != (void *)DriverEntry)
    return (NTSTATUS)0xc000000d;
  U64 *First = MmGetPhysicalMemoryRanges();
  Retained = MmGetPhysicalMemoryRanges();
  if (!valid(First) || !valid(Retained) || First == Retained)
    return (NTSTATUS)0xc000000d;
  First[0] = 0x12345678;
  ExFreePool(First);
  if (!valid(Retained))
    return (NTSTATUS)0xc000000d;
  U64 Name[2];
  RtlInitUnicodeString(Name,
                       (const unsigned short *)L"MmGetPhysicalMemoryRanges");
  DynamicQuery = (Query)MmGetSystemRoutineAddress(Name);
  if (!DynamicQuery)
    return (NTSTATUS)0xc000000d;
  NTSTATUS Status = IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device);
  if (Status < 0)
    return Status;
  *(void **)(Driver + 104) = (void *)Unload;
  for (unsigned I = 0; I < 28; ++I)
    *(void **)(Driver + 112 + I * 8) = (void *)Dispatch;
  return 0;
}
