//===- driver_version_query.c - Original query lifecycle fixture
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original version query fixture with a complete buffer oracle.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef NTSTATUS (*Query)(void *);
__declspec(dllimport) NTSTATUS RtlGetVersion(void *);
__declspec(dllimport) void RtlInitUnicodeString(void *, const unsigned short *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(void *);
__declspec(dllimport) NTSTATUS IoCreateDevice(void *, U32, void *, U32, U32, U8,
                                              void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void IofCompleteRequest(void *, U8);
__declspec(dllimport) U32 DbgPrint(const char *, ...);

static void *Device;
static U32 Build;
static Query DynamicQuery;
NTSTATUS DriverEntry(U8 *, void *);
static void *volatile EntryAddress = (void *)DriverEntry;

static NTSTATUS run_query(U32 Size, int Dynamic, U32 *Output) {
  U8 Bytes[320];
  for (unsigned I = 0; I < sizeof(Bytes); ++I)
    Bytes[I] = 0xa5;
  *(U32 *)Bytes = Size;
  if (Size == 276)
    for (unsigned I = 20; I < 276; ++I)
      Bytes[I] = 0;
  NTSTATUS Status = Dynamic ? DynamicQuery(Bytes) : RtlGetVersion(Bytes);
  if (Status != 0 || *(U32 *)Bytes != Size || *(U32 *)(Bytes + 4) != 10 ||
      *(U32 *)(Bytes + 8) != 0 || *(U32 *)(Bytes + 16) != 2)
    return (NTSTATUS)0xc000000d;
  for (unsigned I = 20; I < sizeof(Bytes); ++I)
    if (Bytes[I] != ((Size == 276 && I < 276) ? 0 : 0xa5))
      return (NTSTATUS)0xc000000d;
  *Output = *(U32 *)(Bytes + 12);
  return 0;
}

static NTSTATUS Dispatch(void *Target, U8 *IRP) {
  U8 *Stack = *(U8 **)(IRP + 184);
  U64 Information = 0;
  NTSTATUS Status = Target == Device ? 0 : (NTSTATUS)0xc000000d;
  if (!Status && Stack[0] == 14) {
    U32 Code = *(U32 *)(Stack + 24);
    U32 Value = 0;
    Status = run_query(Code == 0x222004   ? 0
                       : Code == 0x222008 ? 284
                                          : 276,
                       1, &Value);
    if (!Status && Value != Build)
      Status = (NTSTATUS)0xc000000d;
    if (!Status) {
      **(U32 **)(IRP + 24) = Value;
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
  U32 Value;
  if (run_query(276, 1, &Value) || Value != Build)
    __builtin_trap();
  IoDeleteDevice(Device);
  DbgPrint("version query unload complete");
}
NTSTATUS DriverEntry(U8 *Driver, void *RegistryPath) {
  (void)RegistryPath;
  if (EntryAddress != (void *)DriverEntry)
    return (NTSTATUS)0xc000000d;
  NTSTATUS Status = run_query(276, 0, &Build);
  if (Status)
    return Status;
  U64 Name[2];
  RtlInitUnicodeString(Name, (const unsigned short *)L"RtlGetVersion");
  DynamicQuery = (Query)MmGetSystemRoutineAddress(Name);
  if (!DynamicQuery)
    return (NTSTATUS)0xc000000d;
  Status = IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device);
  if (Status < 0)
    return Status;
  *(void **)(Driver + 104) = (void *)Unload;
  for (unsigned I = 0; I < 28; ++I)
    *(void **)(Driver + 112 + I * 8) = (void *)Dispatch;
  return 0;
}
