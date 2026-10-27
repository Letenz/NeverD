//===- driver_registry_values.c - Original registry enumeration fixture ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Enumerates configured values and checks live indices after mutation.
///
//===----------------------------------------------------------------------===//

typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} UNICODE_STRING;
typedef struct {
  U32 Length;
  void *RootDirectory;
  const UNICODE_STRING *ObjectName;
  U32 Attributes;
  void *SecurityDescriptor, *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;
typedef struct {
  U32 TitleIndex, Type, DataOffset, DataLength, NameLength;
  U16 Name[16];
} VALUE_INFORMATION;
_Static_assert(__builtin_offsetof(VALUE_INFORMATION, Name) == 20,
               "x64 full value name");

__declspec(dllimport) NTSTATUS ZwOpenKey(void **, U32, OBJECT_ATTRIBUTES *);
__declspec(dllimport) NTSTATUS ZwEnumerateValueKey(void *, U32, U32, void *,
                                                   U32, U32 *);
__declspec(dllimport) NTSTATUS ZwDeleteValueKey(void *, UNICODE_STRING *);
__declspec(dllimport) NTSTATUS ZwSetValueKey(void *, UNICODE_STRING *, U32, U32,
                                             const void *, U32);
__declspec(dllimport) NTSTATUS ZwClose(void *);
__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);

// Keep an absolute data relocation so the same fixture also exercises rebasing.
static const U16 *volatile DeleteName = (const U16 *)L"Zulu";

NTSTATUS DriverEntry(void *Driver, const UNICODE_STRING *RegistryPath) {
  (void)Driver;
  OBJECT_ATTRIBUTES Attributes = {
      sizeof(Attributes), 0, RegistryPath, 0x240, 0, 0};
  void *Key = 0;
  NTSTATUS Status = ZwOpenKey(&Key, 3, &Attributes);
  if (Status < 0)
    return Status;
  U64 Storage[16];
  VALUE_INFORMATION *Info = (VALUE_INFORMATION *)Storage;
  U32 Required = 0, Count = 0, Sum = 0;
  for (;; ++Count) {
    Status =
        ZwEnumerateValueKey(Key, Count, 1, Storage, sizeof(Storage), &Required);
    if ((U32)Status == 0x8000001a)
      break;
    if (Status < 0 || Count >= 2 || Info->Type != 4 || Info->DataLength != 4 ||
        Info->DataOffset != 32 || Required != 36 ||
        Info->Name[0] != (Count == 0 ? 'Z' : 'A'))
      return (NTSTATUS)0xc000000d;
    Sum += *(U32 *)((char *)Storage + Info->DataOffset);
  }
  if (Count != 2 || Sum != 10)
    return (NTSTATUS)0xc000000d;
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, DeleteName);
  if (ZwDeleteValueKey(Key, &Name) < 0 ||
      ZwSetValueKey(Key, &Name, 0, 4, &Sum, sizeof(Sum)) < 0)
    return (NTSTATUS)0xc000000d;
  Status = ZwEnumerateValueKey(Key, 0, 1, Storage, sizeof(Storage), &Required);
  if (Status < 0 || Info->Name[0] != 'A')
    return (NTSTATUS)0xc000000d;
  Status = ZwEnumerateValueKey(Key, 1, 2, Storage, sizeof(Storage), &Required);
  if (Status < 0 || Required != 16 || ((U32 *)Storage)[3] != 10)
    return (NTSTATUS)0xc000000d;
  return ZwClose(Key);
}
