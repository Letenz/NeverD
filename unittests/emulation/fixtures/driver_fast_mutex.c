//===- driver_fast_mutex.c - Original fast/guarded mutex fixture ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// x64 FAST_MUTEX layout and the documented ExInitializeFastMutex macro.
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  U32 Count, Padding;
  void *Owner;
  U32 Contention, Padding2;
  U64 Event[3];
  U32 OldIrql, Padding3;
} FAST_MUTEX;
_Static_assert(sizeof(FAST_MUTEX) == 56, "x64 fast mutex ABI");
__declspec(dllimport) void KeInitializeEvent(void *, U32, U8);
__declspec(dllimport) void KeInitializeGuardedMutex(FAST_MUTEX *);
__declspec(dllimport) void ExAcquireFastMutex(FAST_MUTEX *);
__declspec(dllimport) U8 ExTryToAcquireFastMutex(FAST_MUTEX *);
__declspec(dllimport) void ExReleaseFastMutex(FAST_MUTEX *);
__declspec(dllimport) void ExAcquireFastMutexUnsafe(FAST_MUTEX *);
__declspec(dllimport) void ExReleaseFastMutexUnsafe(FAST_MUTEX *);
__declspec(dllimport) void KeAcquireGuardedMutex(FAST_MUTEX *);
__declspec(dllimport) U8 KeTryToAcquireGuardedMutex(FAST_MUTEX *);
__declspec(dllimport) void KeReleaseGuardedMutex(FAST_MUTEX *);
__declspec(dllimport) void KeAcquireGuardedMutexUnsafe(FAST_MUTEX *);
__declspec(dllimport) void KeReleaseGuardedMutexUnsafe(FAST_MUTEX *);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) void *KeGetCurrentThread(void);
__declspec(dllimport) U8 KeAreAllApcsDisabled(void);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) void KeLowerIrql(U8);
__declspec(dllimport) void KeEnterCriticalRegion(void);
__declspec(dllimport) void KeLeaveCriticalRegion(void);
__declspec(dllimport) void KeEnterGuardedRegion(void);
__declspec(dllimport) void KeLeaveGuardedRegion(void);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePool(void *);
static FAST_MUTEX *Locks[2];
static NTSTATUS exercise(FAST_MUTEX *M, U32 Guarded, U8 Irql) {
  U8 Previous = KfRaiseIrql(Irql);
  for (U32 Try = 0; Try != 2; ++Try) {
    M->OldIrql = 0xa5a5a5a5;
    if (Guarded) {
      if (Try) {
        if (!KeTryToAcquireGuardedMutex(M))
          return (NTSTATUS)0xc000000d;
      } else
        KeAcquireGuardedMutex(M);
    } else {
      if (Try) {
        if (!ExTryToAcquireFastMutex(M))
          return (NTSTATUS)0xc000000d;
      } else
        ExAcquireFastMutex(M);
    }
    if (M->Count || M->Owner != KeGetCurrentThread() || M->OldIrql != Irql ||
        KeGetCurrentIrql() != 1 || !KeAreAllApcsDisabled() ||
        ExTryToAcquireFastMutex(M) || KeTryToAcquireGuardedMutex(M))
      return (NTSTATUS)0xc000000d;
    if (Guarded)
      KeReleaseGuardedMutex(M);
    else
      ExReleaseFastMutex(M);
    if (M->Count != 1 || M->Owner || KeGetCurrentIrql() != Irql)
      return (NTSTATUS)0xc000000d;
  }
  M->OldIrql = 0x12345678;
  if (Guarded) {
    KeEnterGuardedRegion();
    KeAcquireGuardedMutexUnsafe(M);
  } else {
    KeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(M);
  }
  if (M->Count || M->Owner != KeGetCurrentThread() ||
      M->OldIrql != 0x12345678 || KeGetCurrentIrql() != Irql)
    return (NTSTATUS)0xc000000d;
  if (Guarded) {
    KeReleaseGuardedMutexUnsafe(M);
    KeLeaveGuardedRegion();
  } else {
    ExReleaseFastMutexUnsafe(M);
    KeLeaveCriticalRegion();
  }
  if (M->Count != 1 || M->Owner || M->OldIrql != 0x12345678 ||
      KeGetCurrentIrql() != Irql || M->Padding != 0xa5a5a5a5 ||
      M->Padding2 != 0xa5a5a5a5 || M->Padding3 != 0xa5a5a5a5 || M->Contention)
    return (NTSTATUS)0xc000000d;
  KeLowerIrql(Previous);
  return 0;
}
static void unload(void *Driver) {
  (void)Driver;
  ExFreePool(Locks[0]);
  ExFreePool(Locks[1]);
}
static void (*volatile Unload)(void *) = unload;
NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)RegistryPath;
  for (U32 I = 0; I != 2; ++I) {
    FAST_MUTEX *M = ExAllocatePoolWithTag(0, sizeof(FAST_MUTEX), 0x74784d46);
    if (!M)
      return (NTSTATUS)0xc000009a;
    Locks[I] = M;
    for (U32 B = 0; B != sizeof(FAST_MUTEX); ++B)
      ((U8 *)M)[B] = 0xa5;
    if (I)
      KeInitializeGuardedMutex(M);
    else {
      M->Count = 1;
      M->Owner = 0;
      M->Contention = 0;
      KeInitializeEvent(M->Event, 1, 0);
    }
    // Each initialization form supports both public API families.
    for (U32 Guarded = 0; Guarded != 2; ++Guarded)
      for (U8 Irql = 0; Irql != 2; ++Irql) {
        NTSTATUS Status = exercise(M, Guarded, Irql);
        if (Status)
          return Status;
      }
  }
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  return 0;
}
