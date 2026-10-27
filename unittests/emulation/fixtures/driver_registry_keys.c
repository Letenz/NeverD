//===- driver_registry_keys.c - Original key information fixture ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Queries key metadata, enumerates immediate children and observes deletion.
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
  U64 LastWriteTime;
  U32 TitleIndex, ClassOffset, ClassLength, SubKeys, MaxNameLength;
  U32 MaxClassLength, Values, MaxValueNameLength, MaxValueDataLength;
} KEY_FULL_INFORMATION;
typedef struct {
  U64 LastWriteTime;
  U32 TitleIndex, NameLength;
  U16 Name[32];
} KEY_BASIC_INFORMATION;
_Static_assert(__builtin_offsetof(KEY_FULL_INFORMATION, MaxValueDataLength) ==
                   40,
               "x64 full key fields");
_Static_assert(__builtin_offsetof(KEY_BASIC_INFORMATION, Name) == 16,
               "x64 basic key name");
__declspec(dllimport) NTSTATUS ZwOpenKey(void **, U32, OBJECT_ATTRIBUTES *);
__declspec(dllimport) NTSTATUS ZwQueryKey(void *, U32, void *, U32, U32 *);
__declspec(dllimport) NTSTATUS ZwEnumerateKey(void *, U32, U32, void *, U32,
                                              U32 *);
__declspec(dllimport) NTSTATUS ZwDeleteKey(void *);
__declspec(dllimport) NTSTATUS ZwClose(void *);
__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);
static const U16 *volatile ChildName = (const U16 *)L"Longest";

NTSTATUS DriverEntry(void *Driver, const UNICODE_STRING *RegistryPath) {
  (void)Driver;
  OBJECT_ATTRIBUTES Attributes = {
      sizeof(Attributes), 0, RegistryPath, 0x240, 0, 0};
  void *Key = 0;
  NTSTATUS Status = ZwOpenKey(&Key, 9, &Attributes);
  if (Status < 0)
    return Status;
  U32 Required = 0;
  if ((U32)ZwQueryKey(Key, 2, 0, 0, &Required) != 0xc0000023 || Required != 44)
    return (NTSTATUS)0xc000000d;
  KEY_FULL_INFORMATION Full;
  Status = ZwQueryKey(Key, 2, &Full, sizeof(Full), &Required);
  if (Status < 0 || Full.LastWriteTime != 123 || Full.SubKeys != 2 ||
      Full.MaxNameLength != 14 || Full.Values != 1 ||
      Full.MaxValueDataLength != 4)
    return (NTSTATUS)0xc000000d;
  KEY_BASIC_INFORMATION Basic;
  for (U32 I = 0; I < 2; ++I) {
    Status = ZwEnumerateKey(Key, I, 0, &Basic, sizeof(Basic), &Required);
    if (Status < 0 || Basic.LastWriteTime != 1000 ||
        Basic.Name[0] != (I == 0 ? 'A' : 'L'))
      return (NTSTATUS)0xc000000d;
  }
  if ((U32)ZwEnumerateKey(Key, 2, 0, &Basic, sizeof(Basic), &Required) !=
      0x8000001a)
    return (NTSTATUS)0xc000000d;
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, ChildName);
  Attributes.RootDirectory = Key;
  Attributes.ObjectName = &Name;
  void *Child = 0;
  Status = ZwOpenKey(&Child, 0x10000, &Attributes);
  if (Status < 0 || ZwDeleteKey(Child) < 0 || ZwClose(Child) < 0)
    return (NTSTATUS)0xc000000d;
  Status = ZwQueryKey(Key, 2, &Full, sizeof(Full), &Required);
  if (Status < 0 || Full.LastWriteTime != 1000 || Full.SubKeys != 1 ||
      Full.MaxNameLength != 14)
    return (NTSTATUS)0xc000000d;
  return ZwClose(Key);
}
