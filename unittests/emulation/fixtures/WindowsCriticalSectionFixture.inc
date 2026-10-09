//===- WindowsCriticalSectionFixture.inc - Critical-section lifetime ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef struct {
  void *Debug;
  long Lock, Depth;
  void *Owner, *Semaphore;
  U64 Spin;
} CriticalSection;
_Static_assert(sizeof(CriticalSection) == 40, "critical-section layout");
static void criticalSections(void *Kernel, void *Native, U32 Mode) {
  void (*Initialize)(CriticalSection *) =
      lookup(Kernel, "InitializeCriticalSection");
  int (*InitializeSpin)(CriticalSection *, U32) =
      lookup(Kernel, "InitializeCriticalSectionAndSpinCount");
  long (*NativeInitialize)(CriticalSection *) =
      lookup(Native, "RtlInitializeCriticalSection");
  long (*NativeInitializeSpin)(CriticalSection *, U32) =
      lookup(Native, "RtlInitializeCriticalSectionAndSpinCount");
  void (*Enter)(CriticalSection *) = lookup(Kernel, "EnterCriticalSection");
  long (*NativeEnter)(CriticalSection *) =
      lookup(Native, "RtlEnterCriticalSection");
  int (*Try)(CriticalSection *) = lookup(Kernel, "TryEnterCriticalSection");
  U8 (*NativeTry)(CriticalSection *) =
      lookup(Native, "RtlTryEnterCriticalSection");
  void (*Leave)(CriticalSection *) = lookup(Kernel, "LeaveCriticalSection");
  long (*NativeLeave)(CriticalSection *) =
      lookup(Native, "RtlLeaveCriticalSection");
  void (*Delete)(CriticalSection *) = lookup(Kernel, "DeleteCriticalSection");
  long (*NativeDelete)(CriticalSection *) =
      lookup(Native, "RtlDeleteCriticalSection");
  U32 (*Thread)(void) = lookup(Kernel, "GetCurrentThreadId");
  const U64 ThreadID = Thread();
  struct {
    U64 Before;
    CriticalSection CS;
    U64 After;
  } Object;
  Object.Before = 0x12345678abcdef01ull;
  Object.After = 0xfedcba9876543210ull;
  if (Mode != CriticalSectionsMode) {
    CriticalSection CS = {0};
    if (Mode == CriticalUninitializedMode) {
      NativeEnter(&CS);
      return;
    }
    Initialize(&CS);
    if (Mode == CriticalUnownedLeaveMode)
      NativeLeave(&CS);
    if (Mode == CriticalReinitializeMode)
      NativeInitialize(&CS);
    if (Mode == CriticalDeletedMode) {
      Delete(&CS);
      NativeEnter(&CS);
    }
    if (Mode == CriticalCorruptedMode) {
      CS.Owner = (void *)(ThreadID + 1);
      Enter(&CS);
    }
    if (Mode == CriticalOwnedDeleteMode) {
      Enter(&CS);
      NativeDelete(&CS);
    }
    return;
  }
  for (U32 Kind = 0; Kind != 4; ++Kind) {
    CriticalSection *CS = &Object.CS;
    if (Kind == 0)
      Initialize(CS);
    else if (Kind == 1)
      require(InitializeSpin(CS, 0x12345) != 0, 71);
    else if (Kind == 2)
      require(NativeInitialize(CS) == 0, 72);
    else
      require(NativeInitializeSpin(CS, 0x12345) == 0, 73);
    require(CS->Lock == -1 && CS->Depth == 0 && CS->Owner == 0, 74);
    Enter(CS);
    require(CS->Lock == -2 && CS->Depth == 1 && (U64)CS->Owner == ThreadID, 75);
    require(NativeEnter(CS) == 0, 76);
    require(CS->Lock == -2 && CS->Depth == 2 && (U64)CS->Owner == ThreadID, 77);
    require(Try(CS) != 0 && NativeTry(CS) != 0 && CS->Depth == 4, 78);
    Leave(CS);
    require(NativeLeave(CS) == 0 && CS->Depth == 2 &&
                (U64)CS->Owner == ThreadID,
            79);
    require(NativeLeave(CS) == 0, 80);
    Leave(CS);
    require(CS->Lock == -1 && CS->Depth == 0 && CS->Owner == 0, 81);
    if (Kind & 1)
      Delete(CS);
    else
      require(NativeDelete(CS) == 0, 82);
    require(Object.Before == 0x12345678abcdef01ull &&
                Object.After == 0xfedcba9876543210ull,
            83);
  }
}
