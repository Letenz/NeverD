//===- driver_namespace.c - Original named-object driver fixture --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise namespace lookup, data imports and held-link/device deletion.
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} STRING;
typedef struct {
  U32 Length;
  U64 Root;
  STRING *Name;
  U32 Attributes;
  void *Security, *Quality;
} ATTRIBUTES;
__declspec(dllimport) void *IoDriverObjectType;
__declspec(dllimport) void *ObGetObjectType(void *);
__declspec(dllimport) NTSTATUS ObReferenceObjectByName(STRING *, U32, void *,
                                                       U32, void *, U8, void *,
                                                       void **);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(U64, U32, void *, U8,
                                                         void **, void *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) NTSTATUS ObQueryNameString(void *, void *, U32, U32 *);
__declspec(dllimport) NTSTATUS ZwOpenDirectoryObject(U64 *, U32, ATTRIBUTES *);
__declspec(dllimport) NTSTATUS ZwOpenSymbolicLinkObject(U64 *, U32,
                                                        ATTRIBUTES *);
__declspec(dllimport) NTSTATUS ZwQuerySymbolicLinkObject(U64, STRING *, U32 *);
__declspec(dllimport) NTSTATUS ZwQueryObject(U64, U32, void *, U32, U32 *);
__declspec(dllimport) NTSTATUS ZwClose(U64);
__declspec(dllimport) NTSTATUS IoCreateDevice(void *, U32, STRING *, U32, U32,
                                              U8, void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) NTSTATUS IoCreateSymbolicLink(STRING *, STRING *);
__declspec(dllimport) NTSTATUS IoDeleteSymbolicLink(STRING *);
#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0010000u + __LINE__);                               \
  } while (0)
#define TEXT(N, T)                                                             \
  static U16 N##Bytes[] = T;                                                   \
  STRING N = {sizeof(N##Bytes) - 2, sizeof(N##Bytes), N##Bytes}
static int equal(STRING *A, STRING *B) {
  if (A->Length != B->Length)
    return 0;
  for (U32 I = 0; I != A->Length / 2; ++I)
    if (A->Buffer[I] != B->Buffer[I])
      return 0;
  return 1;
}
static void unload(void *Driver) { (void)Driver; }
static void (*volatile Unload)(void *) = unload;

NTSTATUS DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  STRING *DriverName = (STRING *)((U8 *)Driver + 0x38);
  void *Object = 0;
  CHECK(!ObReferenceObjectByName(DriverName, 0x40, 0, 0, IoDriverObjectType, 0,
                                 0, &Object));
  CHECK(Object == Driver);
  ObfDereferenceObject(Object);
  _Alignas(8) U8 Information[256];
  U32 Required = 0;
  CHECK((U32)ObQueryNameString(Driver, 0, 0, &Required) == 0xc0000004u);
  CHECK(Required == sizeof(STRING) + DriverName->Length + 2);
  CHECK(
      !ObQueryNameString(Driver, Information, sizeof(Information), &Required));
  CHECK(equal((STRING *)Information, DriverName));

  TEXT(Directory, L"\\GLOBAL??");
  TEXT(Link, L"\\DosDevices\\OriginalNamespace");
  TEXT(RelativeLink, L"OriginalNamespace");
  TEXT(DeviceName, L"\\Device\\OriginalNamespace");
  TEXT(OtherName, L"\\Device\\OriginalReplacement");
  void *Device;
  CHECK(!IoCreateDevice(Driver, 0, &DeviceName, 0x22, 0x100, 0, &Device));
  CHECK(!IoCreateSymbolicLink(&Link, &DeviceName));
  ATTRIBUTES Attributes = {sizeof(ATTRIBUTES), 0, &Directory, 0x240, 0, 0};
  U64 Root, Handle;
  CHECK(!ZwOpenDirectoryObject(&Root, 0x80000000u, &Attributes));
  CHECK(!ZwQueryObject(Root, 1, Information, sizeof(Information), 0));
  CHECK(equal((STRING *)Information, &Directory));
  Attributes.Root = Root;
  Attributes.Name = &RelativeLink;
  CHECK(!ZwOpenSymbolicLinkObject(&Handle, 0x80000000u, &Attributes));
  CHECK(!ObReferenceObjectByHandle(Handle, 0, 0, 0, &Object, 0));
  U16 TargetBytes[64];
  STRING Target = {7, 0, TargetBytes};
  CHECK((U32)ZwQuerySymbolicLinkObject(Handle, &Target, &Required) ==
        0xc0000023u);
  CHECK(Required == DeviceName.MaximumLength && Target.Length == 7);
  Target.MaximumLength = Required;
  CHECK(!ZwQuerySymbolicLinkObject(Handle, &Target, &Required));
  CHECK(equal(&Target, &DeviceName));
  CHECK(!IoDeleteSymbolicLink(&Link));
  CHECK((U32)IoCreateSymbolicLink(&Link, &OtherName) == 0xc0000035u);
  CHECK(!ZwClose(Handle));
  CHECK(!IoCreateSymbolicLink(&Link, &OtherName));
  CHECK(!ZwOpenSymbolicLinkObject(&Handle, 0, &Attributes));
  Target.MaximumLength = sizeof(TargetBytes);
  CHECK(!ZwQuerySymbolicLinkObject(Handle, &Target, 0));
  CHECK(equal(&Target, &OtherName));
  CHECK(
      !ObQueryNameString(Object, Information, sizeof(Information), &Required));
  TEXT(DetachedName, L"\\OriginalNamespace");
  CHECK(equal((STRING *)Information, &DetachedName));
  ObfDereferenceObject(Object);
  CHECK(!IoDeleteSymbolicLink(&Link));
  CHECK(!ZwClose(Handle));
  CHECK(!ZwClose(Root));

  CHECK(!ObReferenceObjectByName(&DeviceName, 0x40, 0, 0,
                                 ObGetObjectType(Device), 0, 0, &Object));
  CHECK(Object == Device);
  IoDeleteDevice(Device);
  CHECK(!*(void **)((U8 *)Driver + 8));
  CHECK(
      !ObQueryNameString(Object, Information, sizeof(Information), &Required));
  CHECK(equal((STRING *)Information, &DetachedName));
  ObfDereferenceObject(Object);
  return 0;
}
