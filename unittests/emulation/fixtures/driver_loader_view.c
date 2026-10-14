//===- driver_loader_view.c - Original declared loader view fixture -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent x64 byte accesses through entry, request and unload callbacks.
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;

__declspec(dllimport) NTSTATUS IoCreateDevice(void *, U32, void *, U32, U32, U8,
                                              void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void IofCompleteRequest(void *, U8);
__declspec(dllimport) U32 DbgPrint(const char *, ...);

static U8 *Object;
static void *Device;
static volatile U8 *Loader;
static U32 InitialFlags;
// Keep a real DIR64 relocation for the independent rebasing control.
NTSTATUS DriverEntry(U8 *, void *);
static void *volatile EntryAddress = (void *)DriverEntry;

static U64 qword(const volatile U8 *P, unsigned Offset) {
  return *(const volatile U64 *)(P + Offset);
}
static U32 word(const volatile U8 *P, unsigned Offset) {
  return *(const volatile U32 *)(P + Offset);
}
static int consistent(void) {
  return qword(Object, 40) == (U64)Loader &&
         qword(Loader, 48) == qword(Object, 24) &&
         qword(Loader, 56) == qword(Object, 88) &&
         qword(Loader, 56) == (U64)EntryAddress &&
         word(Loader, 64) == word(Object, 32) &&
         word(Loader, 104) == InitialFlags;
}
static NTSTATUS Dispatch(void *Target, U8 *IRP) {
  NTSTATUS Status = 0;
  U64 Information = 0;
  U8 *Stack = (U8 *)qword(IRP, 184);
  if (Target != Device || !consistent())
    Status = (NTSTATUS)0xc000000d;
  else if (Stack[0] == 14) {
    U32 *Output = (U32 *)qword(IRP, 24);
    U32 Code = word(Stack, 24);
    if (Code == 0x222004)
      *Output = Loader[68]; // Unsupported padding, never inferred zero.
    else if (Code == 0x222008)
      *(volatile U32 *)(Loader + 104) = 0; // Explicitly read-only.
    else if (Code == 0x22200c)
      *Output = (U32)qword(Loader, 104); // Straddles the flags field.
    else
      *Output = word(Loader, 104);
    Information = 4;
  }
  *(NTSTATUS *)(IRP + 48) = Status;
  *(U64 *)(IRP + 56) = Information;
  IofCompleteRequest(IRP, 0);
  return Status;
}
static void Unload(void *Driver) {
  if (Driver != Object || !consistent())
    __builtin_trap();
  IoDeleteDevice(Device);
  DbgPrint("loader view unload complete");
}
NTSTATUS DriverEntry(U8 *Driver, void *RegistryPath) {
  (void)RegistryPath;
  Object = Driver;
  Loader = (volatile U8 *)qword(Driver, 40);
  InitialFlags = word(Loader, 104);
  if (!consistent())
    return (NTSTATUS)0xc000000d;
  NTSTATUS Status = IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device);
  if (Status < 0)
    return Status;
  *(void **)(Driver + 104) = (void *)Unload;
  for (unsigned I = 0; I < 28; ++I)
    *(void **)(Driver + 112 + I * 8) = (void *)Dispatch;
  return 0;
}
