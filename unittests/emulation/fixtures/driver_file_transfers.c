//===- driver_file_transfers.c - Original native file transfer fixture ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise synchronous buffered file handles from another file's IOCTL.

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned long long U64;
#ifdef NEVERD_WDK_FIXTURE
#include <ntifs.h>
typedef unsigned long U32;
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PIRP PACKET;
typedef PUNICODE_STRING REGISTRY;
typedef IO_STATUS_BLOCK STATUS_BLOCK;
typedef LARGE_INTEGER OFFSET;
typedef UNICODE_STRING NAME;
_Static_assert(IRP_MJ_LOCK_CONTROL == 0x11, "lock control major");
_Static_assert(IRP_MJ_FLUSH_BUFFERS == 9, "flush major");
_Static_assert(IRP_MJ_FILE_SYSTEM_CONTROL == 13, "filesystem control major");
_Static_assert(FIELD_OFFSET(IRP, UserIosb) == 0x48, "x64 IOSB");
_Static_assert(FIELD_OFFSET(IRP, UserBuffer) == 0x70, "x64 user buffer");
_Static_assert(FIELD_OFFSET(IRP, Tail.Overlay.Thread) == 0x98,
               "issuing thread");
_Static_assert(FIELD_OFFSET(IO_STACK_LOCATION, Parameters.Read.Length) == 8,
               "read length");
_Static_assert(FIELD_OFFSET(IO_STACK_LOCATION, Parameters.Write.Key) == 16,
               "write key");
_Static_assert(FIELD_OFFSET(IO_STACK_LOCATION, Parameters.Read.ByteOffset) ==
                   24,
               "read offset");
_Static_assert(FIELD_OFFSET(FILE_OBJECT, CurrentByteOffset) == 0x68,
               "stored file position");
#else
typedef unsigned int U32;
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef void *PACKET;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} NAME, *REGISTRY;
typedef struct {
  union {
    NTSTATUS Status;
    void *Pointer;
  };
  U64 Information;
} STATUS_BLOCK;
typedef union {
  long long QuadPart;
} OFFSET;
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER, U32, void *, U32, U32, U8,
                                              DEVICE *);
__declspec(dllimport) void IoDeleteDevice(DEVICE);
__declspec(dllimport) void IofCompleteRequest(PACKET, U8);
__declspec(dllimport) void ObfReferenceObject(void *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) void *IoAllocateWorkItem(DEVICE);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(DEVICE, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) void *PsGetCurrentProcessId(void);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) U8 KeAreApcsDisabled(void);
__declspec(dllimport) U8 KeAreAllApcsDisabled(void);
__declspec(dllimport) void KeEnterGuardedRegion(void);
__declspec(dllimport) void KeLeaveGuardedRegion(void);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) void KeLowerIrql(U8);
__declspec(dllimport) NTSTATUS ZwReadFile(void *, void *, void *, void *,
                                          STATUS_BLOCK *, void *, U32, OFFSET *,
                                          U32 *);
__declspec(dllimport) NTSTATUS ZwWriteFile(void *, void *, void *, void *,
                                           STATUS_BLOCK *, void *, U32,
                                           OFFSET *, U32 *);
__declspec(dllimport) NTSTATUS ZwSetInformationFile(void *, STATUS_BLOCK *,
                                                    void *, U32, U32);
__declspec(dllimport) NTSTATUS ZwFlushBuffersFile(void *, STATUS_BLOCK *);
__declspec(dllimport) NTSTATUS ZwDeviceIoControlFile(void *, void *, void *,
                                                     void *, STATUS_BLOCK *,
                                                     U32, void *, U32, void *,
                                                     U32);
__declspec(dllimport) NTSTATUS ZwFsControlFile(void *, void *, void *, void *,
                                               STATUS_BLOCK *, U32, void *, U32,
                                               void *, U32);
__declspec(dllimport) NTSTATUS ZwQueryInformationFile(void *, STATUS_BLOCK *,
                                                      void *, U32, U32);
__declspec(dllimport) NTSTATUS ObOpenObjectByPointer(void *, U32, void *, U32,
                                                     void *, U8, void **);
__declspec(dllimport) NTSTATUS ZwClose(void *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(NAME *);
#endif
typedef NTSTATUS (*TRANSFER)(void *, void *, void *, void *, STATUS_BLOCK *,
                             void *, U32, OFFSET *, U32 *);

typedef NTSTATUS (*SET_INFORMATION)(void *, STATUS_BLOCK *, void *, U32, U32);
typedef NTSTATUS (*FLUSH_FILE)(void *, STATUS_BLOCK *);
typedef NTSTATUS (*CONTROL_FILE)(void *, void *, void *, void *, STATUS_BLOCK *,
                                 U32, void *, U32, void *, U32);
#define POSITION 0x123456789ull
#define FLUSH_INFORMATION 0x100000007ull

#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0060000u + __LINE__);                               \
  } while (0)

static DEVICE Device;
static void *File, *CallerThread, *Work;
static PACKET Pending;
static U8 *Buffer;
static STATUS_BLOCK *IOSB;
static U32 Creates, Transfers, Flushes, Controls, Closes, Failed, Index,
    ControlIndex;
static U64 PendingInformation;
static U16 Mode;
static TRANSFER ReadDynamic, WriteDynamic;
static SET_INFORMATION SetDynamic;
static FLUSH_FILE FlushDynamic;
static CONTROL_FILE DeviceControlDynamic, FsControlDynamic;

static U32 completionStatus(void) {
  return Mode == 'W'                                 ? 0x80000005u
         : Mode == 'E' || Mode == 'R' || Mode == 'M' ? 0xc0000001u
                                                     : 0;
}
static NTSTATUS complete(PACKET Irp, U32 Status, U64 Information) {
  *(U32 *)((U8 *)Irp + 0x30) = Status;
  *(U64 *)((U8 *)Irp + 0x38) = Information;
  IofCompleteRequest(Irp, 0);
  return (NTSTATUS)Status;
}
static void worker(DEVICE Owner, void *Context) {
  if (Owner != Device || Context != File ||
      PsGetCurrentThread() == CallerThread || (U64)PsGetCurrentProcessId() != 4)
    Failed = 1;
  IoFreeWorkItem(Work);
  complete(Pending, Failed ? 0xc006ffff : completionStatus(),
           PendingInformation);
}
static NTSTATUS flush(PACKET Irp) {
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  CHECK(Stack[0] == 9 && Stack[1] == 0);
  CHECK(*(void **)(Stack + 0x30) == File);
  CHECK(*(void **)((U8 *)Irp + 0xc0) == File);
  CHECK(*(void **)((U8 *)Irp + 0x98) == CallerThread);
  CHECK(*(void **)((U8 *)Irp + 0x48) == IOSB);
  CHECK(*(U32 *)((U8 *)Irp + 0x10) == 0x60000);
  CHECK(*((U8 *)Irp + 0x47) == 2 && *((U8 *)Irp + 0x40) == 0);
  CHECK(!*(void **)((U8 *)Irp + 0x18) && !*(void **)((U8 *)Irp + 0x70));
  CHECK(!*(void **)((U8 *)Irp + 0x50));
  CHECK(KeAreApcsDisabled() && !KeAreAllApcsDisabled());
  ++Flushes;
  PendingInformation = FLUSH_INFORMATION;
  if (Mode == 'P' || Mode == 'W' || Mode == 'R' || Mode == 'U') {
    Pending = Irp;
    Work = IoAllocateWorkItem(Device);
    CHECK(Work);
    Stack[3] |= 1;
    IoQueueWorkItem(Work, worker, 1, File);
    return 0x103;
  }
  U8 Old = 0;
  if (Mode == 'G')
    KeEnterGuardedRegion();
  if (Mode == 'L') {
#ifdef NEVERD_WDK_FIXTURE
    KeRaiseIrql(DISPATCH_LEVEL, &Old);
#else
    Old = KfRaiseIrql(2);
#endif
  }
  complete(Irp, completionStatus(), FLUSH_INFORMATION);
  if (Mode == 'G' || Mode == 'L')
    CHECK(IOSB->Information == 0xa5a5a5a5a5a5a5a5ull);
  if (Mode == 'G')
    KeLeaveGuardedRegion();
  if (Mode == 'L')
    KeLowerIrql(Old);
  // Nondeferred completion has consumed Irp. Only caller storage remains.
  CHECK(IOSB->Information == (Mode == 'E' || Mode == 'M' ? 0xa5a5a5a5a5a5a5a5ull
                                                         : FLUSH_INFORMATION));
  return Mode == 'M' ? 0 : (NTSTATUS)completionStatus();
}
static NTSTATUS transfer(PACKET Irp) {
  PendingInformation = Index == 2 || Index == 3 ? 0 : 7;
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  const U8 Write = Stack[0] == 4;
  const U32 Length = Index == 2 || Index == 3 ? 0 : 16;
  CHECK(*(void **)(Stack + 0x30) == File);
  CHECK(*(void **)((U8 *)Irp + 0xc0) == File);
  CHECK(*(void **)((U8 *)Irp + 0x98) == CallerThread);
  CHECK(PsGetCurrentThread() == CallerThread);
  CHECK((U64)PsGetCurrentProcessId() == 0x2000);
  CHECK(KeAreApcsDisabled() && !KeAreAllApcsDisabled());
  CHECK(*(U64 *)((U8 *)File + 0x68) == POSITION);
  CHECK(*(U32 *)((U8 *)Irp + 0x10) ==
        (0x60000u |
         (Write ? (Length ? 0xa30u : 0xa10u) : (Length ? 0x970u : 0x950u))));
  CHECK(*((U8 *)Irp + 0x47) == 2 && *((U8 *)Irp + 0x40) == 0);
  CHECK(*(void **)((U8 *)Irp + 0x48) == IOSB);
  CHECK(!*(void **)((U8 *)Irp + 0x50));
  CHECK(*(void **)((U8 *)Irp + 0x70) == (!Write && Length ? Buffer : 0));
  CHECK(*(U32 *)(Stack + 8) == Length);
  CHECK(*(U32 *)(Stack + 16) == (Index < 2 ? 0x89abcdefu : 0));
  const U64 ExpectedOffset = Index < 2                  ? 0x100000025ull + Index
                             : Index == 3 || Index == 5 ? ~0ull
                                                        : POSITION;
  CHECK(*(U64 *)(Stack + 24) == ExpectedOffset);
  U8 *System = *(U8 **)((U8 *)Irp + 0x18);
  CHECK(Length ? System && System != Buffer : !System);
  if (Length)
    for (U32 I = 0; I < 7; ++I) {
      if (Write)
        CHECK(System[I] == 0xa5);
      System[I] = (U8)(0x20 + I);
    }
  ++Transfers;
  if (Mode == 'P' || Mode == 'W' || Mode == 'R' || Mode == 'U') {
    Pending = Irp;
    Work = IoAllocateWorkItem(Device);
    CHECK(Work);
    Stack[3] |= 1;
    IoQueueWorkItem(Work, worker, 1, File);
    return 0x103;
  }
  U8 Old = 0;
  if (Mode == 'G')
    KeEnterGuardedRegion();
  if (Mode == 'L') {
#ifdef NEVERD_WDK_FIXTURE
    KeRaiseIrql(DISPATCH_LEVEL, &Old);
#else
    Old = KfRaiseIrql(2);
#endif
  }
  complete(Irp, completionStatus(), Length ? 7 : 0);
  if (Mode == 'G')
    KeLeaveGuardedRegion();
  if (Mode == 'L')
    KeLowerIrql(Old);
  CHECK(IOSB->Information == 0xa5a5a5a5a5a5a5a5ull && Buffer[0] == 0xa5);
  CHECK(*(U32 *)((U8 *)Irp + 0x30) == completionStatus());
  return Mode == 'M' ? 0 : (NTSTATUS)completionStatus();
}
static NTSTATUS control(PACKET Irp) {
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  const U8 FS = ControlIndex >= 4;
  const U32 Shape = ControlIndex % 4;
  const U32 Input = Shape == 1 || Shape == 3 ? 0 : 16;
  const U32 Output = Shape >= 2 ? 0 : 16;
  const U32 Length = Input > Output ? Input : Output;
  CHECK(Stack[0] == (FS ? 13 : 14) && Stack[1] == 0 && Stack[2] == 0);
  CHECK(*(void **)(Stack + 0x30) == File);
  CHECK(*(U32 *)(Stack + 8) == Output && *(U32 *)(Stack + 0x10) == Input);
  CHECK(*(U32 *)(Stack + 0x18) == 0x22e000 + 4 * Shape);
  CHECK(!*(void **)(Stack + 0x20) && !*(void **)((U8 *)Irp + 8));
  CHECK(*(void **)((U8 *)Irp + 0xc0) == File);
  CHECK(*(void **)((U8 *)Irp + 0x98) == CallerThread);
  CHECK(*(void **)((U8 *)Irp + 0x48) == IOSB);
  CHECK(*(void **)((U8 *)Irp + 0x70) == (Length ? Buffer : 0));
  CHECK(!*(void **)((U8 *)Irp + 0x50));
  CHECK(
      *(U32 *)((U8 *)Irp + 0x10) ==
      (0x60000u | (FS ? 0x800u : 0) | (Length ? (Output ? 0x70u : 0x30u) : 0)));
  CHECK(*((U8 *)Irp + 0x47) == 2 && *((U8 *)Irp + 0x40) == 0);
  CHECK(PsGetCurrentThread() == CallerThread &&
        (U64)PsGetCurrentProcessId() == 0x2000);
  CHECK(KeAreApcsDisabled() && !KeAreAllApcsDisabled());
  U8 *System = *(U8 **)((U8 *)Irp + 0x18);
  CHECK(Length ? System && System != Buffer : !System);
  for (U32 I = 0; I < Length; ++I) {
    CHECK(System[I] == (Input ? 0x30 + I : 0));
    System[I] = (U8)(0x20 + I);
  }
  ++Controls;
  PendingInformation = Output ? 7 : FLUSH_INFORMATION;
  if (Mode == 'P' || Mode == 'W' || Mode == 'R' || Mode == 'U') {
    Pending = Irp;
    Work = IoAllocateWorkItem(Device);
    CHECK(Work);
    Stack[3] |= 1;
    IoQueueWorkItem(Work, worker, 1, File);
    return 0x103;
  }
  U8 Old = 0;
  if (Mode == 'G')
    KeEnterGuardedRegion();
  if (Mode == 'L') {
#ifdef NEVERD_WDK_FIXTURE
    KeRaiseIrql(DISPATCH_LEVEL, &Old);
#else
    Old = KfRaiseIrql(2);
#endif
  }
  complete(Irp, completionStatus(), PendingInformation);
  if (Mode == 'G' || Mode == 'L')
    CHECK(IOSB->Information == 0xa5a5a5a5a5a5a5a5ull && Buffer[0] == 0xa5);
  if (Mode == 'G')
    KeLeaveGuardedRegion();
  if (Mode == 'L')
    KeLowerIrql(Old);
  const U8 Published = !FS && Mode != 'E' && Mode != 'M';
  CHECK(IOSB->Information ==
        (Published ? PendingInformation : 0xa5a5a5a5a5a5a5a5ull));
  CHECK(Buffer[0] == (Published && Output ? 0x20 : 0xa5));
  // Only FSCTL retains its immediate packet until the service tail.
  if (FS)
    CHECK(*(U32 *)((U8 *)Irp + 0x30) == completionStatus());
  return Mode == 'M' ? 0 : (NTSTATUS)completionStatus();
}
static NTSTATUS handle(PACKET Irp) {
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  if (Stack[0] == 0) {
    if (++Creates == 1) {
      File = *(void **)(Stack + 0x30);
      ObfReferenceObject(File);
    }
  } else if (Stack[0] == 2) {
    ++Closes;
  } else if (Stack[0] == 14) {
    CHECK(Creates == 2 && *(void **)(Stack + 0x30) != File);
    CallerThread = PsGetCurrentThread();
    void *Handle = 0, *Append = 0;
    CHECK(!ObOpenObjectByPointer(File, 0x200, 0, 0, 0, 0, &Handle));
    CHECK(!ObOpenObjectByPointer(File, 0x200, 0, 4, 0, 0, &Append));
    U64 Position = 0x11111111;
    STATUS_BLOCK PositionStatus;
    CHECK(!ZwSetInformationFile(Handle, &PositionStatus, &Position, 8, 14));
    CHECK(!PositionStatus.Status && !PositionStatus.Information);
    Position = POSITION;
    CHECK(!SetDynamic(Handle, &PositionStatus, &Position, 8, 14));
    Position = 0;
    CHECK(!ZwQueryInformationFile(Handle, &PositionStatus, &Position, 8, 14));
    CHECK(Position == POSITION && PositionStatus.Information == 8);
    void *FlushHandle = 0;
    CHECK(!ObOpenObjectByPointer(File, 0x200, 0, 2, 0, 0, &FlushHandle));
    for (U32 I = 0; I < 2; ++I) {
      STATUS_BLOCK FlushStatus;
      IOSB = Mode == 'U' ? (STATUS_BLOCK *)(*(U8 **)((U8 *)Irp + 0x70) + 32)
                         : &FlushStatus;
      IOSB->Pointer = (void *)0xa5a5a5a5a5a5a5a5ull;
      IOSB->Information = 0xa5a5a5a5a5a5a5a5ull;
      U32 Returned = I ? (U32)FlushDynamic(FlushHandle, IOSB)
                       : (U32)ZwFlushBuffersFile(FlushHandle, IOSB);
      CHECK(Returned == (Mode == 'M' ? 0 : completionStatus()));
      const U8 Published = Mode != 'E' && Mode != 'M';
      CHECK((U32)IOSB->Status ==
            (Published ? completionStatus() : 0xa5a5a5a5u));
      CHECK(IOSB->Information ==
            (Published ? FLUSH_INFORMATION : 0xa5a5a5a5a5a5a5a5ull));
      CHECK(*((U32 *)IOSB + 1) == 0xa5a5a5a5u);
      CHECK(*(U64 *)((U8 *)File + 0x68) == POSITION);
      CHECK(!KeAreApcsDisabled() && !KeAreAllApcsDisabled());
    }
    CHECK(!ZwClose(FlushHandle));
    for (ControlIndex = 0; ControlIndex < 8; ++ControlIndex) {
      const U8 FS = ControlIndex >= 4;
      const U32 Shape = ControlIndex % 4;
      const U32 Input = Shape == 1 || Shape == 3 ? 0 : 16;
      const U32 Output = Shape >= 2 ? 0 : 16;
      U8 InputBytes[16], OutputBytes[16];
      STATUS_BLOCK ControlStatus;
      Buffer = Mode == 'U' ? *(U8 **)((U8 *)Irp + 0x70) : OutputBytes;
      IOSB = Mode == 'U' ? (STATUS_BLOCK *)(Buffer + 32) : &ControlStatus;
      for (U32 I = 0; I < 16; ++I) {
        InputBytes[I] = (U8)(0x30 + I);
        Buffer[I] = 0xa5;
      }
      IOSB->Pointer = (void *)0xa5a5a5a5a5a5a5a5ull;
      IOSB->Information = 0xa5a5a5a5a5a5a5a5ull;
      const U32 Code = 0x22e000 + 4 * Shape;
      U32 Returned;
      if (ControlIndex & 1)
        Returned = (U32)(FS ? FsControlDynamic : DeviceControlDynamic)(
            Handle, 0, 0, 0, IOSB, Code, InputBytes, Input, Buffer, Output);
      else if (FS)
        Returned = (U32)ZwFsControlFile(Handle, 0, 0, 0, IOSB, Code, InputBytes,
                                        Input, Buffer, Output);
      else
        Returned = (U32)ZwDeviceIoControlFile(
            Handle, 0, 0, 0, IOSB, Code, InputBytes, Input, Buffer, Output);
      CHECK(Returned == (Mode == 'M' ? 0 : completionStatus()));
      const U8 Published = Mode != 'E' && Mode != 'M';
      CHECK((U32)IOSB->Status ==
            (Published ? completionStatus() : 0xa5a5a5a5u));
      CHECK(IOSB->Information == (Published ? (Output ? 7 : FLUSH_INFORMATION)
                                            : 0xa5a5a5a5a5a5a5a5ull));
      CHECK(*((U32 *)IOSB + 1) == 0xa5a5a5a5u);
      const U8 Copied = Output && (completionStatus() >> 30) != 3;
      CHECK(Buffer[0] == (Copied ? 0x20 : 0xa5));
      CHECK(Buffer[6] == (Copied ? 0x26 : 0xa5) && Buffer[7] == 0xa5);
      for (U32 I = 0; I < 16; ++I)
        CHECK(InputBytes[I] == 0x30 + I);
      CHECK(*(U64 *)((U8 *)File + 0x68) == POSITION && !Failed);
      CHECK(PsGetCurrentThread() == CallerThread &&
            (U64)PsGetCurrentProcessId() == 0x2000);
      CHECK(!KeAreApcsDisabled() && !KeAreAllApcsDisabled());
    }
    for (Index = 0; Index < 6; ++Index) {
      const U8 Write = Index & 1;
      const U32 Length = Index == 2 || Index == 3 ? 0 : 16;
      U64 Storage[8];
      Buffer = Mode == 'U' ? *(U8 **)((U8 *)Irp + 0x70) : (U8 *)Storage;
      for (U32 I = 0; I < 64; ++I)
        Buffer[I] = 0xa5;
      STATUS_BLOCK Status;
      IOSB = Index == 4 ? (STATUS_BLOCK *)(Buffer + 4) : &Status;
      IOSB->Pointer = (void *)0xa5a5a5a5a5a5a5a5ull;
      IOSB->Information = 0xa5a5a5a5a5a5a5a5ull;
      OFFSET Offset;
      Offset.QuadPart = Index < 2    ? 0x100000025ll + Index
                        : Index == 3 ? -1
                        : Index == 5 ? -3
                                     : -2;
      U32 Key = 0x89abcdef;
      OFFSET *OffsetArgument = Index == 2 ? 0 : &Offset;
      U32 *KeyArgument = Index < 2 ? &Key : 0;
      void *CurrentHandle = Index == 5 ? Append : Handle;
      U32 Returned;
      if (Index < 2)
        Returned = Write
                       ? (U32)ZwWriteFile(CurrentHandle, 0, 0, 0, IOSB, Buffer,
                                          Length, OffsetArgument, KeyArgument)
                       : (U32)ZwReadFile(CurrentHandle, 0, 0, 0, IOSB, Buffer,
                                         Length, OffsetArgument, KeyArgument);
      else
        Returned = (U32)(Write ? WriteDynamic : ReadDynamic)(
            CurrentHandle, 0, 0, 0, IOSB, Buffer, Length, OffsetArgument,
            KeyArgument);
      CHECK(Returned == (Mode == 'M' ? 0 : completionStatus()));
      const U8 Published = Mode != 'E' && Mode != 'M';
      CHECK((U32)IOSB->Status ==
            (Published ? completionStatus() : 0xa5a5a5a5u));
      CHECK(IOSB->Information ==
            (Published ? (Length ? 7u : 0u) : 0xa5a5a5a5a5a5a5a5ull));
      CHECK(*((U32 *)IOSB + 1) == 0xa5a5a5a5u);
      const U8 Copied = !Write && Length && (completionStatus() >> 30) != 3;
      CHECK(Buffer[0] == (Copied ? 0x20 : 0xa5));
      CHECK(Buffer[3] == (Copied ? 0x23 : 0xa5) && Buffer[31] == 0xa5);
      CHECK(PsGetCurrentThread() == CallerThread);
      CHECK((U64)PsGetCurrentProcessId() == 0x2000);
      CHECK(!KeAreApcsDisabled() && !KeAreAllApcsDisabled());
      CHECK(*(U64 *)((U8 *)File + 0x68) == POSITION && !Failed);
    }
    ObfDereferenceObject(File);
    CHECK(!ZwClose(Append) && !ZwClose(Handle));
  } else {
    CHECK(Stack[0] == 18 || (Stack[0] == 0x11 && Stack[1] == 3));
  }
  return 0;
}
static NTSTATUS dispatch(DEVICE Owner, PACKET Irp) {
  CHECK(Owner == Device);
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  if (Stack[0] == 3 || Stack[0] == 4)
    return transfer(Irp);
  if (Stack[0] == 9)
    return flush(Irp);
  if (Stack[0] == 13 || (Stack[0] == 14 && *(void **)(Stack + 0x30) == File))
    return control(Irp);
  return complete(Irp, (U32)handle(Irp), 0);
}
static NTSTATUS (*volatile Dispatch)(DEVICE, PACKET) = dispatch;
static void unload(DRIVER Driver) {
  (void)Driver;
  if (Creates == 2 && Transfers == 6 && Flushes == 2 && Controls == 8 &&
      Closes == 2 && !Failed)
    IoDeleteDevice(Device);
}
static void (*volatile Unload)(DRIVER) = unload;
NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  Mode = Registry->Length ? Registry->Buffer[Registry->Length / 2 - 1] : 'S';
  static U16 ReadName[] = {'Z', 'w', 'R', 'e', 'a', 'd', 'F', 'i', 'l', 'e', 0};
  static U16 WriteName[] = {'Z', 'w', 'W', 'r', 'i', 't',
                            'e', 'F', 'i', 'l', 'e', 0};
  static U16 SetName[] = {'Z', 'w', 'S', 'e', 't', 'I', 'n', 'f', 'o', 'r', 'm',
                          'a', 't', 'i', 'o', 'n', 'F', 'i', 'l', 'e', 0};
  static U16 FlushName[] = {'Z', 'w', 'F', 'l', 'u', 's', 'h', 'B', 'u', 'f',
                            'f', 'e', 'r', 's', 'F', 'i', 'l', 'e', 0};
  static U16 DeviceControlName[] = {'Z', 'w', 'D', 'e', 'v', 'i', 'c', 'e',
                                    'I', 'o', 'C', 'o', 'n', 't', 'r', 'o',
                                    'l', 'F', 'i', 'l', 'e', 0};
  static U16 FsControlName[] = {'Z', 'w', 'F', 's', 'C', 'o', 'n', 't',
                                'r', 'o', 'l', 'F', 'i', 'l', 'e', 0};
  NAME Name = {sizeof(ReadName) - 2, sizeof(ReadName), ReadName};
  ReadDynamic = (TRANSFER)MmGetSystemRoutineAddress(&Name);
  Name.Length = sizeof(WriteName) - 2;
  Name.MaximumLength = sizeof(WriteName);
  Name.Buffer = WriteName;
  WriteDynamic = (TRANSFER)MmGetSystemRoutineAddress(&Name);
  Name.Length = sizeof(SetName) - 2;
  Name.MaximumLength = sizeof(SetName);
  Name.Buffer = SetName;
  SetDynamic = (SET_INFORMATION)MmGetSystemRoutineAddress(&Name);
  Name.Length = sizeof(FlushName) - 2;
  Name.MaximumLength = sizeof(FlushName);
  Name.Buffer = FlushName;
  FlushDynamic = (FLUSH_FILE)MmGetSystemRoutineAddress(&Name);
  Name.Length = sizeof(DeviceControlName) - 2;
  Name.MaximumLength = sizeof(DeviceControlName);
  Name.Buffer = DeviceControlName;
  DeviceControlDynamic = (CONTROL_FILE)MmGetSystemRoutineAddress(&Name);
  Name.Length = sizeof(FsControlName) - 2;
  Name.MaximumLength = sizeof(FsControlName);
  Name.Buffer = FsControlName;
  FsControlDynamic = (CONTROL_FILE)MmGetSystemRoutineAddress(&Name);
  CHECK(ReadDynamic && WriteDynamic && SetDynamic && FlushDynamic &&
        DeviceControlDynamic && FsControlDynamic);
  CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device));
  *(U32 *)((U8 *)Device + 0x30) |= 4;
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  const U32 Majors[] = {0, 2, 3, 4, 9, 13, 0x11, 14, 18};
  for (U32 I = 0; I < sizeof(Majors) / sizeof(Majors[0]); ++I)
    *(NTSTATUS(**)(DEVICE, PACKET))((U8 *)Driver + 0x70 + 8 * Majors[I]) =
        Dispatch;
  return 0;
}
