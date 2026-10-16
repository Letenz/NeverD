//===- driver_filesystem_provider.c - Original volume identity fixture ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe declared devices and open existing files through real WDM callbacks.

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned long U32;
typedef unsigned long long U64;
#ifdef NEVERD_WDK_FIXTURE
#include <ntifs.h>
// Target exports whose declarations are omitted by this WDK.
NTKERNELAPI POBJECT_TYPE ObGetObjectType(PVOID Object);
NTKERNELAPI NTSTATUS ObReferenceObjectByName(
    PUNICODE_STRING Name, ULONG Attributes, PACCESS_STATE AccessState,
    ACCESS_MASK DesiredAccess, POBJECT_TYPE Type, KPROCESSOR_MODE Mode,
    PVOID ParseContext, PVOID *Object);
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PUNICODE_STRING REGISTRY;
typedef UNICODE_STRING TEXT;
typedef OBJECT_ATTRIBUTES OPENATTR;
typedef IO_STATUS_BLOCK IOSTATUS;
typedef PIRP PACKET;
typedef PFILE_OBJECT FILEPTR;
_Static_assert(FIELD_OFFSET(IO_STACK_LOCATION, Parameters.Create.Options) ==
                   0x10,
               "create option ABI");
_Static_assert(FIELD_OFFSET(IO_SECURITY_CONTEXT, DesiredAccess) == 0x10,
               "create access ABI");
_Static_assert(FIELD_OFFSET(DEVICE_OBJECT, AlignmentRequirement) == 0x98,
               "device alignment ABI");
_Static_assert(FIELD_OFFSET(DEVICE_OBJECT, SectorSize) == 0x130,
               "device sector ABI");
_Static_assert(FIELD_OFFSET(DEVICE_OBJECT, DeviceType) == 0x48,
               "device type ABI");
_Static_assert(FIELD_OFFSET(DRIVER_OBJECT, DriverUnload) == 0x68,
               "driver unload ABI");
_Static_assert(sizeof(FILE_BASIC_INFORMATION) == 40,
               "basic file information ABI");
_Static_assert(sizeof(FILE_STANDARD_INFORMATION) == 24,
               "standard file information ABI");
_Static_assert(sizeof(FILE_NETWORK_OPEN_INFORMATION) == 56,
               "network file information ABI");
#else
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef void *HANDLE;
typedef void *PACKET;
typedef void *FILEPTR;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} TEXT, *REGISTRY;
typedef struct {
  U32 Length;
  HANDLE RootDirectory;
  TEXT *ObjectName;
  U32 Attributes;
  void *SecurityDescriptor, *SecurityQualityOfService;
} OPENATTR;
typedef struct {
  U64 Status, Information;
} IOSTATUS;
__declspec(dllimport) void *ObGetObjectType(void *);
__declspec(dllimport) NTSTATUS ObReferenceObjectByName(TEXT *, U32, void *, U32,
                                                       void *, U8, void *,
                                                       void **);
__declspec(dllimport) void RtlInitUnicodeString(TEXT *, const U16 *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER, U32, void *, U32, U32, U8,
                                              DEVICE *);
__declspec(dllimport) void IoDeleteDevice(DEVICE);
__declspec(dllimport) DEVICE IoAttachDeviceToDeviceStack(DEVICE, DEVICE);
__declspec(dllimport) void IoDetachDevice(DEVICE);
__declspec(dllimport) DEVICE IoGetAttachedDeviceReference(DEVICE);
__declspec(dllimport) DEVICE IoGetDeviceAttachmentBaseRef(DEVICE);
__declspec(dllimport) NTSTATUS IoGetDeviceObjectPointer(TEXT *, U32, FILEPTR *,
                                                        DEVICE *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(TEXT *);
__declspec(dllimport) U8 KeAreApcsDisabled(void);
__declspec(dllimport) U8 KeAreAllApcsDisabled(void);
__declspec(dllimport) void *IoAllocateWorkItem(DEVICE);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(DEVICE, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) NTSTATUS ZwOpenFile(HANDLE *, U32, OPENATTR *, IOSTATUS *,
                                          U32, U32);
__declspec(dllimport) NTSTATUS ZwCreateFile(HANDLE *, U32, OPENATTR *,
                                            IOSTATUS *, void *, U32, U32, U32,
                                            U32, void *, U32);
__declspec(dllimport) NTSTATUS IoCreateFile(HANDLE *, U32, OPENATTR *,
                                            IOSTATUS *, void *, U32, U32, U32,
                                            U32, void *, U32, U32, void *, U32);
__declspec(dllimport) void IoCancelFileOpen(DEVICE, FILEPTR);
__declspec(dllimport) NTSTATUS IofCallDriver(DEVICE, PACKET);
__declspec(dllimport) NTSTATUS ZwClose(HANDLE);
__declspec(dllimport) void ObfReferenceObject(void *);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) NTSTATUS ZwQueryInformationFile(HANDLE, IOSTATUS *,
                                                      void *, U32, U32);
__declspec(dllimport) NTSTATUS IoQueryFileInformation(void *, U32, U32, void *,
                                                      U32 *);
#endif
#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0080000u + __LINE__);                               \
  } while (0)
static DEVICE Guest, Volume;
static DRIVER Owner;
static void *Work;
static volatile U32 Done, Failed;
static U16 Mode;
static HANDLE HeldHandle;
static FILEPTR OpenedFile, HeldFile;
static PACKET PendingOpen;
static U8 PendingMajor;
static U32 OpenCreates, OpenCleanups, OpenCloses;

static NTSTATUS cancelCompletion(DEVICE Device, PACKET Irp, void *Context) {
  FILEPTR File = *(FILEPTR *)((U8 *)Irp + 0xc0);
  CHECK(Device == Guest && Context == Volume);
  // Reenter our CLEANUP dispatch while the CREATE completion is suspended.
  IoCancelFileOpen(Guest, File);
  CHECK((*(U32 *)((U8 *)File + 0x50) & 0x204000) == 0x204000);
  *(U32 *)((U8 *)Irp + 0x30) = 0xc0000022;
  *(U64 *)((U8 *)Irp + 0x38) = 0;
  return 0;
}
static NTSTATUS forwardOpen(PACKET Irp, U8 Major) {
  U8 *Current = *(U8 **)((U8 *)Irp + 0xb8);
  U8 *Next = Current - 0x48;
  for (U32 I = 0; I != 0x48; ++I)
    Next[I] = Current[I];
  Next[3] = 0;
  if (Major == 0 && Mode == 'X') {
    Next[3] = 0xe0;
    *(NTSTATUS(**)(DEVICE, PACKET, void *))(Next + 0x38) = cancelCompletion;
    *(void **)(Next + 0x40) = Volume;
  }
  return IofCallDriver(Volume, Irp);
}
static void openWorker(DEVICE Device, void *Context) {
  if (Device != Guest || Context != PendingOpen ||
      forwardOpen(PendingOpen, PendingMajor))
    Failed = 1;
  IoFreeWorkItem(Work);
  Done = 1;
}
static NTSTATUS fileDispatch(DEVICE Device, PACKET Irp) {
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  U8 Major = Stack[0];
  FILEPTR File = *(FILEPTR *)(Stack + 0x30);
  CHECK(Device == Guest && File == *(FILEPTR *)((U8 *)Irp + 0xc0));
  U8 Pointer = Mode == 'B' || Mode == 'V' || Mode == 'K' || Mode == 'T';
  if (Pointer && (Major == 0 || Major == 18))
    CHECK(KeAreApcsDisabled() && !KeAreAllApcsDisabled());
  if (!Major) {
    U8 *Security = *(U8 **)(Stack + 8);
    CHECK(*(U32 *)((U8 *)Irp + 0x10) == 0x884);
    CHECK(*(void **)((U8 *)Irp + 0x98) == PsGetCurrentThread());
    CHECK((*(U32 *)(Stack + 0x10) >> 24) == 1);
    CHECK(*(U32 *)(Security + 0x10) & 0x100000);
    CHECK(*(void **)(Security + 8));
    OpenedFile = File;
    ++OpenCreates;
    if (Pointer) {
      CHECK(*(U32 *)(Stack + 0x10) == 0x1000040);
      CHECK(*(U16 *)(Stack + 0x1a) == 3);
    }
    if ((Mode == 'P' || Mode == 'V') && !Done) {
      PendingOpen = Irp;
      PendingMajor = Major;
      Stack[3] |= 1;
      Work = IoAllocateWorkItem(Guest);
      CHECK(Work);
      IoQueueWorkItem(Work, openWorker, 1, Irp);
      return 0x103;
    }
  } else if (Major == 5) {
    CHECK(Mode == 'Q' && File == OpenedFile);
    CHECK(*(U32 *)((U8 *)Irp + 0x10) == 0x870 ||
          *(U32 *)((U8 *)Irp + 0x10) == 0x10);
  } else if (Major == 18) {
    ++OpenCleanups;
    if (Mode == 'K' && !Done) {
      PendingOpen = Irp;
      PendingMajor = Major;
      Stack[3] |= 1;
      Work = IoAllocateWorkItem(Guest);
      CHECK(Work);
      IoQueueWorkItem(Work, openWorker, 1, Irp);
      return 0x103;
    }
    if (Mode == 'X') {
      CHECK(*(U32 *)((U8 *)Irp + 0x10) == 0x404);
      CHECK(*(void **)((U8 *)Irp + 0x48) == (U8 *)Irp + 0x30);
      CHECK(*(void **)((U8 *)Irp + 0x50));
      CHECK(*(void **)((U8 *)Irp + 0x98) == PsGetCurrentThread());
    }
  } else {
    CHECK(Major == 2);
    ++OpenCloses;
  }
  return forwardOpen(Irp, Major);
}
static NTSTATUS metadata(HANDLE Handle) {
  const U32 Classes[] = {4, 5, 6, 9, 34, 35};
  const U32 Sizes[] = {40, 24, 8, 14, 56, 8};
  for (U32 Native = 0; Native != 2; ++Native)
    for (U32 I = 0; I != 6; ++I) {
      U64 Buffer[8];
      IOSTATUS Status;
      U32 Returned = 0xdeadbeef;
      for (U32 J = 0; J != 8; ++J)
        Buffer[J] = 0xa5a5a5a5a5a5a5a5ull;
      NTSTATUS Code =
          Native ? ZwQueryInformationFile(Handle, &Status, Buffer,
                                          sizeof(Buffer), Classes[I])
                 : IoQueryFileInformation(OpenedFile, Classes[I],
                                          sizeof(Buffer), Buffer, &Returned);
      CHECK(!Code && (Native ? Status.Information : Returned) == Sizes[I]);
      CHECK(((U8 *)Buffer)[Sizes[I]] == 0xa5);
      if (Classes[I] == 4 || Classes[I] == 34) {
        CHECK(Buffer[0] == 1 && Buffer[1] == 2 && Buffer[2] == 3 &&
              Buffer[3] == 4);
        CHECK(*(U32 *)((U8 *)Buffer + (Classes[I] == 4 ? 32 : 48)) == 0x20);
      }
      if (Classes[I] == 5) {
        CHECK(Buffer[0] == 4096 && Buffer[1] == 4);
        CHECK(*(U32 *)((U8 *)Buffer + 16) == 1 && ((U8 *)Buffer)[20] == 0 &&
              ((U8 *)Buffer)[21] == 0);
      }
      if (Classes[I] == 6)
        CHECK(Buffer[0] == ~0ull);
      if (Classes[I] == 9) {
        CHECK(*(U32 *)Buffer == 10);
        CHECK(((U16 *)Buffer)[2] == '\\' && ((U16 *)Buffer)[3] == 'D');
      }
      if (Classes[I] == 34)
        CHECK(Buffer[4] == 4096 && Buffer[5] == 4);
    }
  U64 Buffer[8];
  IOSTATUS Status;
  *(U64 *)&Status = 0xa5a5a5a5a5a5a5a5ull;
  Status.Information = 0xa5a5a5a5a5a5a5a5ull;
  CHECK((U32)ZwQueryInformationFile(Handle, &Status, Buffer, 23, 5) ==
        0xc0000004);
  CHECK(*(U64 *)&Status == 0xa5a5a5a5a5a5a5a5ull &&
        Status.Information == 0xa5a5a5a5a5a5a5a5ull);
  CHECK((U32)ZwQueryInformationFile(Handle, &Status, Buffer, 9, 9) ==
        0x80000005);
  CHECK(Status.Information == 8 && *(U32 *)Buffer == 10);
  const U32 Local[] = {8, 14, 16, 17, 51};
  const U32 Values[] = {0x100081, 0, 0x20, 511, 0};
  for (U32 I = 0; I != 5; ++I) {
    Buffer[0] = 0;
    CHECK(!ZwQueryInformationFile(Handle, &Status, Buffer, sizeof(Buffer),
                                  Local[I]));
    CHECK(*(U32 *)Buffer == Values[I]);
  }
  return 0;
}
static NTSTATUS openCases(void) {
  TEXT Name;
  OPENATTR Attr = {0};
  IOSTATUS Status;
  HANDLE Handle = (HANDLE)(U64)0x12345678;
  Attr.Length = sizeof(Attr);
  Attr.Attributes = 0x240;
  Attr.ObjectName = &Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"\\Device\\DeclaredVolume\\Data");
  *(U64 *)&Status = 0x8877665544332211ULL;
  Status.Information = 0x9988;
  NTSTATUS Returned = ZwOpenFile(&Handle, 0x100081, &Attr, &Status, 7, 0x60);
  if (Mode == 'X') {
    CHECK((U32)Returned == 0xc0000022 && Handle == (HANDLE)(U64)0x12345678);
    CHECK(*(U64 *)&Status == 0x8877665544332211ULL);
    CHECK(Status.Information == 0x9988 && OpenCleanups == 1 && OpenCloses == 1);
    Mode = 'C';
    Returned = ZwOpenFile(&Handle, 0x100081, &Attr, &Status, 7, 0x60);
  }
  CHECK(!Returned && Handle && !*(U32 *)&Status && Status.Information == 1);
  if (Mode == 'Q')
    CHECK(!metadata(Handle));
  if (Mode == 'P')
    CHECK(Done && !Failed);
  if (Mode == 'U') {
    HeldHandle = Handle;
    HeldFile = OpenedFile;
    ObfReferenceObject(HeldFile);
    return 0;
  }
  CHECK(!ZwClose(Handle));
  CHECK(
      !ZwCreateFile(&Handle, 0x100081, &Attr, &Status, 0, 0, 7, 1, 0x60, 0, 0));
  CHECK(!ZwClose(Handle));
  // An indirect call keeps the final three arguments in their x64 stack slots.
  __typeof__(&IoCreateFile) volatile Create = IoCreateFile;
  CHECK(!Create(&Handle, 0x100081, &Attr, &Status, 0, 0, 7, 1, 0x60, 0, 0, 0, 0,
                0x200));
  CHECK(!ZwClose(Handle));
  if (Mode == 'R') {
    RtlInitUnicodeString(&Name, (const U16 *)L"\\Device\\DeclaredVolume\\");
    CHECK(!ZwOpenFile(&Attr.RootDirectory, 0x100081, &Attr, &Status, 7, 0x21));
    RtlInitUnicodeString(&Name, (const U16 *)L"Data");
    CHECK(!ZwOpenFile(&Handle, 0x100081, &Attr, &Status, 7, 0x60));
    CHECK(!ZwClose(Attr.RootDirectory));
    CHECK(!ZwClose(Handle));
  }
  CHECK(OpenCreates == OpenCleanups && OpenCleanups == OpenCloses);
  return 0;
}
static void fileUnload(DRIVER Driver) {
  if (Driver != Owner || Failed)
    return;
  if (HeldHandle) {
    if (ZwClose(HeldHandle))
      return;
    if (OpenCloses || OpenCleanups != 1)
      return;
    ObfDereferenceObject(HeldFile);
  } else if (HeldFile) {
    if (OpenCloses || OpenCleanups != 1)
      return;
    ObfDereferenceObject(HeldFile);
  }
  if (OpenCreates != OpenCleanups || OpenCleanups != OpenCloses)
    return;
  IoDetachDevice(Volume);
  IoDeleteDevice(Guest);
}

static NTSTATUS pointerCases(void) {
  TEXT Name;
  FILEPTR File = 0;
  DEVICE Device = 0;
  typedef NTSTATUS (*OPENPOINTER)(TEXT *, U32, FILEPTR *, DEVICE *);
  OPENPOINTER Open = IoGetDeviceObjectPointer;
  if (Mode == 'T') {
    RtlInitUnicodeString(&Name, (const U16 *)L"IoGetDeviceObjectPointer");
    Open = (OPENPOINTER)MmGetSystemRoutineAddress(&Name);
    CHECK(Open);
  }
  RtlInitUnicodeString(&Name, (const U16 *)L"\\Device\\DeclaredVolume\\Data");
  CHECK(!KeAreApcsDisabled());
  CHECK(!Open(&Name, 0x100081, &File, &Device));
  CHECK(File == OpenedFile && Device == Guest);
  CHECK(!KeAreApcsDisabled() && !KeAreAllApcsDisabled());
  CHECK(OpenCreates == 1 && OpenCleanups == 1 && OpenCloses == 0);
  CHECK((*(U32 *)((U8 *)File + 0x50) & 0x4002) == 0x4000);
  if (Mode == 'V' || Mode == 'K')
    CHECK(Done && !Failed);
  if (Mode == 'T')
    HeldFile = File;
  else {
    ObfDereferenceObject(File);
    CHECK(OpenCloses == 1);
  }
  return 0;
}

static NTSTATUS referenceVolume(DEVICE *Result) {
  TEXT Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"\\Device\\DeclaredVolume");
  return ObReferenceObjectByName(&Name, 0x40, 0, 0, ObGetObjectType(Guest), 0,
                                 0, (void **)Result);
}
static NTSTATUS probe(void) {
  DEVICE Observed = 0;
  CHECK(!referenceVolume(&Observed) && Observed == Volume);
  CHECK(*(U16 *)Volume == 3);
  CHECK(*(DRIVER *)((U8 *)Volume + 8) != Owner);
  CHECK(*(U32 *)((U8 *)Volume + 0x30) == 0);
  CHECK(*(U32 *)((U8 *)Volume + 0x48) == 8);
  CHECK(*(U32 *)((U8 *)Volume + 0x98) == 511);
  CHECK(*(U16 *)((U8 *)Volume + 0x130) == 4096);
  CHECK(ObGetObjectType(Guest) == ObGetObjectType(Volume));
  ObfDereferenceObject(Observed);
  CHECK(IoGetAttachedDeviceReference(Volume) == Volume);
  ObfDereferenceObject(Volume);
  CHECK(IoGetDeviceAttachmentBaseRef(Volume) == Volume);
  ObfDereferenceObject(Volume);
  return 0;
}
static void worker(DEVICE Device, void *Context) {
  Failed = Device != Guest || Context != Volume || probe();
  IoFreeWorkItem(Work);
  Done = 1;
}
static void unload(DRIVER Driver) {
  if (!Guest)
    return;
  if (Driver == Owner && Done && !Failed && !probe() && Mode != 'G')
    IoDeleteDevice(Guest);
}
static void (*volatile Unload)(DRIVER) = unload;
NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  Owner = Driver;
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  Mode = Registry->Length ? Registry->Buffer[Registry->Length / 2 - 1] : 0;
  CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Guest));
  const NTSTATUS Status = referenceVolume(&Volume);
  U8 Opens = Mode == 'C' || Mode == 'P' || Mode == 'X' || Mode == 'U' ||
             Mode == 'R' || Mode == 'Q' || Mode == 'B' || Mode == 'V' ||
             Mode == 'K' || Mode == 'T';
  if (!Opens && Mode != 'S' && Mode != 'L' && Mode != 'G' && Mode != 'F' &&
      Mode != 'W') {
    CHECK((U32)Status == 0xc0000034u && !Volume);
    IoDeleteDevice(Guest);
    Guest = 0;
    return 0;
  }
  CHECK(!Status && Volume);
  ObfDereferenceObject(Volume);
  CHECK(!probe());
  CHECK(*(DEVICE *)((U8 *)Driver + 8) == Guest);
  CHECK(!*(DEVICE *)((U8 *)Guest + 0x10));
  if (Mode == 'W') {
    *(volatile U32 *)((U8 *)Volume + 0x98) = 0;
    return (NTSTATUS)0xc008ffffu;
  }
  CHECK(IoAttachDeviceToDeviceStack(Guest, Volume) == Volume);
  if (Opens) {
    for (U32 Major = 0; Major != 28; ++Major)
      if (Major == 0 || Major == 2 || Major == 5 || Major == 18)
        *(NTSTATUS(**)(DEVICE, PACKET))((U8 *)Driver + 0x70 + Major * 8) =
            fileDispatch;
    *(void (**)(DRIVER))((U8 *)Driver + 0x68) = fileUnload;
    if (Mode == 'B' || Mode == 'V' || Mode == 'K' || Mode == 'T')
      return pointerCases();
    return openCases();
  }
  CHECK(*(U32 *)((U8 *)Guest + 0x98) == 511);
  CHECK(IoGetDeviceAttachmentBaseRef(Guest) == Volume);
  ObfDereferenceObject(Volume);
  if (Mode == 'F')
    return 0;
  IoDetachDevice(Volume);
  if (Mode == 'L')
    CHECK(IoGetAttachedDeviceReference(Volume) == Volume);
  Work = IoAllocateWorkItem(Guest);
  CHECK(Work);
  IoQueueWorkItem(Work, worker, 1, Volume);
  return 0;
}
