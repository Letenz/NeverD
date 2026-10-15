//===- driver_registry_objects.c - Original key object fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise per-open key identities, shared handles, names and deletion.
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
__declspec(dllimport) NTSTATUS ZwOpenKey(U64 *, U32, ATTRIBUTES *);
__declspec(dllimport) NTSTATUS ZwCreateKey(U64 *, U32, ATTRIBUTES *, U32,
                                           STRING *, U32, U32 *);
__declspec(dllimport) NTSTATUS ZwDeleteKey(U64);
__declspec(dllimport) NTSTATUS ZwQueryKey(U64, U32, void *, U32, U32 *);
__declspec(dllimport) NTSTATUS ZwQueryObject(U64, U32, void *, U32, U32 *);
__declspec(dllimport) NTSTATUS ZwClose(U64);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(U64, U32, void *, U8,
                                                         void **, void *);
__declspec(dllimport) NTSTATUS ObReferenceObjectByPointer(void *, U32, void *,
                                                          U8);
__declspec(dllimport) NTSTATUS ObOpenObjectByPointer(void *, U32, void *, U32,
                                                     void *, U8, U64 *);
__declspec(dllimport) NTSTATUS ObQueryNameString(void *, void *, U32, U32 *);
__declspec(dllimport) void *ObGetObjectType(void *);
__declspec(dllimport) void ObfDereferenceObject(void *);
#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0010000u + __LINE__);                               \
  } while (0)
static void unload(void *Driver) { (void)Driver; }
static void (*volatile Unload)(void *) = unload;
NTSTATUS DriverEntry(void *Driver, STRING *Registry) {
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  static U16 NameBytes[] = L"\\Registry\\Machine\\Software\\OriginalObjects";
  STRING Name = {sizeof(NameBytes) - 2, sizeof(NameBytes), NameBytes};
  ATTRIBUTES Attributes = {sizeof(Attributes), 0, &Name, 0x240, 0, 0};
  U64 First, Second, Clone;
  CHECK(!ZwOpenKey(&First, 0xf003f, &Attributes));
  CHECK(!ZwOpenKey(&Second, 0x80000000u, &Attributes));
  void *A, *B;
  CHECK(!ObReferenceObjectByHandle(First, 0, 0, 0, &A, 0));
  CHECK(!ObReferenceObjectByHandle(Second, 0, 0, 0, &B, 0));
  CHECK(A != B);
  void *Type = ObGetObjectType(A);
  CHECK(Type == ObGetObjectType(B));
  CHECK(!ObReferenceObjectByPointer(A, 0, Type, 0));
  ObfDereferenceObject(A);
  CHECK(!ObOpenObjectByPointer(A, 0x200, 0, 0x80000000u, Type, 0, &Clone));
  _Alignas(8) U8 Information[256];
  U32 Required;
  CHECK((U32)ObQueryNameString(A, 0, 0, &Required) == 0xc0000004u);
  CHECK(Required == 18 + Name.Length);
  CHECK((U32)ObQueryNameString(A, Information, 21, &Required) == 0xc0000004u);
  CHECK(((STRING *)Information)->Length == 3);
  CHECK(((STRING *)Information)->MaximumLength == 3);
  CHECK(((STRING *)Information)->Buffer[0] == '\\');
  CHECK(((STRING *)Information)->Buffer[1] == 0);
  CHECK(!ZwQueryObject(Clone, 1, Information, sizeof(Information), 0));
  STRING *Full = (STRING *)Information;
  CHECK(Full->Length == Name.Length && Full->MaximumLength == Name.Length);
  for (U32 I = 0; I < Name.Length / 2; ++I)
    CHECK(Full->Buffer[I] == Name.Buffer[I]);
  CHECK(!ZwDeleteKey(First));
  CHECK((U32)ZwQueryObject(Second, 1, Information, sizeof(Information),
                           &Required) == 0xc000017cu);
  CHECK(Required == 0);
  U64 Replacement;
  U32 Disposition;
  CHECK(!ZwCreateKey(&Replacement, 1, &Attributes, 0, 0, 0, &Disposition));
  CHECK(Disposition == 1);
  void *New;
  CHECK(!ObReferenceObjectByHandle(Replacement, 0, Type, 0, &New, 0));
  CHECK(New != A && New != B);
  CHECK(!ZwClose(First));
  CHECK(!ZwClose(Second));
  CHECK(!ZwClose(Clone));
  CHECK((U32)ObQueryNameString(A, Information, sizeof(Information),
                               &Required) == 0xc000017cu);
  CHECK(!ObOpenObjectByPointer(A, 0x200, 0, 1, Type, 0, &Clone));
  CHECK((U32)ZwQueryKey(Clone, 0, Information, sizeof(Information),
                        &Required) == 0xc000017cu);
  CHECK(!ZwClose(Clone));
  ObfDereferenceObject(B);
  ObfDereferenceObject(New);
  CHECK(!ZwClose(Replacement));
  // The alternate service name deliberately leaves one caller reference.
  if (Registry->Buffer[Registry->Length / 2 - 1] != 'L')
    ObfDereferenceObject(A);
  return 0;
}
