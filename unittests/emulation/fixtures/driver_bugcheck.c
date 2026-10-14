//===- driver_bugcheck.c - Original callback registration lifecycle -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
// Exercise the model's instruction guard as well as ordinary data accesses.
#pragma comment(linker, "/section:.data,ERW")
typedef struct {
  U64 Links[2];
  void *Callback;
  const char *Component;
  U64 Checksum;
  U32 Reason;
  U8 State;
} RECORD;
_Static_assert(sizeof(RECORD) == 48, "x64 callback record");
__declspec(dllimport) U8 KeRegisterBugCheckReasonCallback(RECORD *, void *, U32,
                                                          const char *);
__declspec(dllimport) U8 KeDeregisterBugCheckReasonCallback(RECORD *);
__declspec(dllimport) NTSTATUS IoCreateDevice(void *, U32, void *, U32, U32, U8,
                                              void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void IofCompleteRequest(void *, U8);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) void KeLowerIrql(U8);
__declspec(dllimport) U8 KeGetCurrentIrql(void);

static RECORD Records[6];
static const char NameA[] = "original callback A";
static const char NameB[] = "original callback B";
static volatile U32 CallbackCount;
static int LeaveRegistered;
static void *Device;
static void CallbackA(U32 Reason, RECORD *Record, void *Data, U32 Length) {
  (void)Reason;
  (void)Record;
  (void)Data;
  (void)Length;
  ++CallbackCount;
}
static void CallbackB(U32 Reason, RECORD *Record, void *Data, U32 Length) {
  CallbackA(Reason, Record, Data, Length);
}
// A genuine DIR64 cell makes alternate-load-address coverage meaningful.
static void *volatile FirstCallback = (void *)CallbackA;
static int consistent(unsigned I, void *Callback, const char *Name, U32 Reason,
                      U8 State) {
  const volatile RECORD *R = &Records[I];
  return !CallbackCount && R->Callback == Callback && R->Component == Name &&
         R->Reason == Reason && R->State == State;
}
static NTSTATUS Dispatch(void *Target, U8 *IRP) {
  U8 *Stack = *(U8 **)(IRP + 184);
  U64 Information = 0;
  if (Target != Device)
    __builtin_trap();
  for (unsigned I = 0; I < 6; ++I)
    if (!consistent(I, CallbackB, NameB, 2 + (I + 1) % 6, 1))
      __builtin_trap();
  if (Stack[0] == 14) {
    U32 Code = *(U32 *)(Stack + 24);
    U32 *Output = *(U32 **)(IRP + 24);
    if (Code == 0x222004)
      *Output = (U32)((volatile RECORD *)Records)->Links[0];
    else if (Code == 0x222008)
      ((volatile RECORD *)Records)->State = 0;
    else if (Code == 0x222010)
      ((void (*)(void))Records)();
    else {
      LeaveRegistered = Code == 0x22200c;
      *Output = 6;
    }
    Information = 4;
  }
  *(NTSTATUS *)(IRP + 48) = 0;
  *(U64 *)(IRP + 56) = Information;
  IofCompleteRequest(IRP, 0);
  return 0;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (!LeaveRegistered)
    for (unsigned I = 0; I < 6; ++I)
      if (!KeDeregisterBugCheckReasonCallback(&Records[I]) ||
          !consistent(I, CallbackB, NameB, 2 + (I + 1) % 6, 0))
        __builtin_trap();
  IoDeleteDevice(Device);
  DbgPrint("callback lifecycle unload complete");
}
NTSTATUS DriverEntry(U8 *Driver, void *RegistryPath) {
  (void)RegistryPath;
  for (unsigned I = 0; I < 6; ++I) {
    U8 Old = 0;
    if (I & 1)
      Old = KfRaiseIrql(2);
    Records[I].State = 0;
    if (KeDeregisterBugCheckReasonCallback(&Records[I]) ||
        !KeRegisterBugCheckReasonCallback(&Records[I], FirstCallback, I + 2,
                                          NameA) ||
        !consistent(I, CallbackA, NameA, I + 2, 1) ||
        KeRegisterBugCheckReasonCallback(&Records[I], CallbackB, 7, NameB) ||
        !consistent(I, CallbackA, NameA, I + 2, 1) ||
        !KeDeregisterBugCheckReasonCallback(&Records[I]) ||
        !consistent(I, CallbackA, NameA, I + 2, 0) ||
        KeDeregisterBugCheckReasonCallback(&Records[I]) ||
        !KeRegisterBugCheckReasonCallback(&Records[I], CallbackB,
                                          2 + (I + 1) % 6, NameB) ||
        KeGetCurrentIrql() != ((I & 1) ? 2 : 0))
      __builtin_trap();
    if (I & 1)
      KeLowerIrql(Old);
  }
  NTSTATUS Status = IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device);
  if (Status < 0)
    return Status;
  *(void **)(Driver + 104) = (void *)Unload;
  for (unsigned I = 0; I < 28; ++I)
    *(void **)(Driver + 112 + I * 8) = (void *)Dispatch;
  return 0;
}
