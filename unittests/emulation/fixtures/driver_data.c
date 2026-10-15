//===- driver_data.c - Original imported kernel data fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} STRING;
__declspec(dllimport) extern void *PsProcessType;
__declspec(dllimport) extern void *PsThreadType;
__declspec(dllimport) extern void *IoDriverObjectType;
__declspec(dllimport) extern void *IoFileObjectType;
__declspec(dllimport) extern void *ExEventObjectType;
__declspec(dllimport) extern void *ExSemaphoreObjectType;
__declspec(dllimport) extern void *PsJobType;
__declspec(dllimport) extern void *SeTokenObjectType;
__declspec(dllimport) extern void *PsInitialSystemProcess;
__declspec(dllimport) extern volatile U8 KeNumberProcessors;
__declspec(dllimport) extern volatile U8 KdDebuggerEnabled;
__declspec(dllimport) extern volatile U8 KdDebuggerNotPresent;
__declspec(dllimport) void *PsGetCurrentProcess(void);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) void *ObGetObjectType(void *);
__declspec(dllimport) NTSTATUS ObReferenceObjectByPointer(void *, U32, void *,
                                                          U8);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(STRING *);
__declspec(dllimport) void RtlCopyMemory(void *, const void *, U64);
__declspec(dllimport) U32 DbgPrint(const char *, ...);

#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)0xc000000d;                                             \
  } while (0)
static void unload(void *Driver) {
  if (ObGetObjectType(Driver) != IoDriverObjectType ||
      ObGetObjectType(PsInitialSystemProcess) != PsProcessType)
    __builtin_trap();
}
static void (*volatile Unload)(void *) = unload;

NTSTATUS DriverEntry(U8 *Driver, const STRING *Registry) {
  *(void (**)(void *))(Driver + 0x68) = Unload;
  const U16 Action = Registry->Buffer[Registry->Length / 2 - 1];
  if (Action == '1') {
    DbgPrint("debugger bytes: %u,%u", (U32)KdDebuggerEnabled,
             (U32)KdDebuggerNotPresent);
    return 0;
  }
  if (Action == '2') {
    *(void *volatile *)&PsProcessType = 0;
    return 0;
  }
  if (Action == '3') {
    ((void (*)(void))&PsProcessType)();
    return 0;
  }
  if (Action == '4') {
    U8 Values[2];
    RtlCopyMemory(Values, (const void *)&KdDebuggerEnabled, 1);
    RtlCopyMemory(Values + 1, (const void *)&KdDebuggerNotPresent, 1);
    DbgPrint("debugger bytes: %u,%u", (U32)Values[0], (U32)Values[1]);
    return 0;
  }
  void *Types[] = {PsProcessType,    PsThreadType,      IoDriverObjectType,
                   IoFileObjectType, ExEventObjectType, ExSemaphoreObjectType,
                   PsJobType,        SeTokenObjectType};
  for (U32 I = 0; I < 8; ++I) {
    CHECK(Types[I]);
    for (U32 J = 0; J < I; ++J)
      CHECK(Types[I] != Types[J]);
  }
  CHECK(PsInitialSystemProcess == PsGetCurrentProcess());
  CHECK(PsProcessType == ObGetObjectType(PsInitialSystemProcess));
  CHECK(PsThreadType == ObGetObjectType(PsGetCurrentThread()));
  CHECK(IoDriverObjectType == ObGetObjectType(Driver));
  CHECK(KeNumberProcessors == 1);
  CHECK(!ObReferenceObjectByPointer(Driver, 0, IoDriverObjectType, 0));
  ObfDereferenceObject(Driver);
  CHECK((U32)ObReferenceObjectByPointer(Driver, 0, IoFileObjectType, 0) ==
        0xc0000024);
  STRING Name;
#define SAME(Symbol)                                                           \
  RtlInitUnicodeString(&Name, (const U16 *)L## #Symbol);                       \
  CHECK(MmGetSystemRoutineAddress(&Name) == (void *)&Symbol)
  SAME(PsProcessType);
  SAME(PsThreadType);
  SAME(IoDriverObjectType);
  SAME(IoFileObjectType);
  SAME(ExEventObjectType);
  SAME(ExSemaphoreObjectType);
  SAME(PsJobType);
  SAME(SeTokenObjectType);
  SAME(PsInitialSystemProcess);
  SAME(KeNumberProcessors);
  SAME(KdDebuggerEnabled);
  SAME(KdDebuggerNotPresent);
#undef SAME
  return 0;
}
