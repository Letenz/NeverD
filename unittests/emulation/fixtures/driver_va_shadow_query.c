//===- driver_va_shadow_query.c - Original mitigation query fixture ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent query footprint and lifecycle oracle.
///
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef NTSTATUS (*Query)(U32, void *, U32, U32 *);
__declspec(dllimport) NTSTATUS NtQuerySystemInformation(U32, void *, U32,
                                                        U32 *);
__declspec(dllimport) NTSTATUS ZwQuerySystemInformation(U32, void *, U32,
                                                        U32 *);
__declspec(dllimport) void RtlInitUnicodeString(void *, const unsigned short *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(void *);
__declspec(dllimport) NTSTATUS IoCreateDevice(void *, U32, void *, U32, U32, U8,
                                              void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void IofCompleteRequest(void *, U8);
__declspec(dllimport) U32 DbgPrint(const char *, ...);

static void *Device;
static U32 DeclaredFlags;
static Query DynamicQuery;
NTSTATUS DriverEntry(U8 *, void *);
static void *volatile EntryAddress = (void *)DriverEntry;

static int verify(Query Function) {
  const U32 Sizes[] = {0, 1, 3, 4, 5, 8};
  for (unsigned I = 0; I < 6; ++I) {
    U32 Area[4] = {0x13579bdf, 0xa5a5a5a5, 0x2468ace0, 0xfedcba98};
    U32 Length[3] = {0x12345678, 0xdeadbeef, 0x87654321};
    NTSTATUS Status = Function(196, Area + 1, Sizes[I], Length + 1);
    if ((U32)Status != (Sizes[I] < 4 ? 0xc0000004u : 0) ||
        Area[1] != (Sizes[I] < 4 ? 0xa5a5a5a5u : DeclaredFlags) ||
        Area[0] != 0x13579bdf || Area[2] != 0x2468ace0 ||
        Area[3] != 0xfedcba98 || Length[0] != 0x12345678 || Length[1] != 4 ||
        Length[2] != 0x87654321)
      return 0;
    Area[1] = 0;
    Status = Function(196, Area + 1, Sizes[I], 0);
    if ((U32)Status != (Sizes[I] < 4 ? 0xc0000004u : 0) ||
        Area[1] != (Sizes[I] < 4 ? 0 : DeclaredFlags) ||
        Area[0] != 0x13579bdf || Area[2] != 0x2468ace0 || Area[3] != 0xfedcba98)
      return 0;
  }
  return 1;
}
static NTSTATUS Dispatch(void *Target, U8 *IRP) {
  U8 *Stack = *(U8 **)(IRP + 184);
  U64 Information = 0;
  NTSTATUS Status = Target == Device ? 0 : (NTSTATUS)0xc000000d;
  if (!Status && Stack[0] == 14) {
    U32 Code = *(U32 *)(Stack + 24);
    U32 Out[2] = {0xa5a5a5a5, 0xa5a5a5a5};
    if (Code == 0x222004)
      Status =
          DynamicQuery(196, Out, 4, Out); // Deliberate overlapping outputs.
    else if (Code == 0x222008)
      Status = DynamicQuery(196, 0, 4, Out); // Deliberately invalid result.
    else if (!verify(DynamicQuery))
      Status = (NTSTATUS)0xc000000d;
    if (!Status) {
      **(U32 **)(IRP + 24) = DeclaredFlags;
      Information = 4;
    }
  }
  *(NTSTATUS *)(IRP + 48) = Status;
  *(U64 *)(IRP + 56) = Information;
  IofCompleteRequest(IRP, 0);
  return Status;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (!verify(NtQuerySystemInformation) || !verify(DynamicQuery))
    __builtin_trap();
  IoDeleteDevice(Device);
  DbgPrint("mitigation query unload complete");
}
NTSTATUS DriverEntry(U8 *Driver, void *RegistryPath) {
  (void)RegistryPath;
  if (EntryAddress != (void *)DriverEntry ||
      NtQuerySystemInformation(196, &DeclaredFlags, 4, 0) != 0 ||
      !verify(NtQuerySystemInformation) || !verify(ZwQuerySystemInformation))
    return (NTSTATUS)0xc000000d;
  U64 Name[2];
  RtlInitUnicodeString(Name,
                       (const unsigned short *)L"ZwQuerySystemInformation");
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
