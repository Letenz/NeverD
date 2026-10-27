//===- driver_qspinlock.c - Original in-stack queued spin lock fixture ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guards a shared counter with an in-stack queued spin lock the way drivers
/// serialize short critical sections, without host or WDK dependencies. The
/// IRQL before, during and after the hold is printed so the test can confirm
/// the raise and restore.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef U64 KSPIN_LOCK;
// x64 wdm.h KLOCK_QUEUE_HANDLE: KSPIN_LOCK_QUEUE{Next, Lock} then KIRQL
// OldIrql.
typedef struct {
  void *Next;
  void *Lock;
  U64 OldIrql;
} KLOCK_QUEUE_HANDLE;

__declspec(dllimport) U64 DbgPrint(const char *, ...);
__declspec(dllimport) void KeInitializeSpinLock(KSPIN_LOCK *);
__declspec(dllimport) void KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *,
                                                          KLOCK_QUEUE_HANDLE *);
__declspec(dllimport) void KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *);
__declspec(dllimport) U8 KeGetCurrentIrql(void);

static KSPIN_LOCK Lock;
static U64 Shared;

NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  KLOCK_QUEUE_HANDLE Handle;
  KeInitializeSpinLock(&Lock);
  U8 Before = KeGetCurrentIrql();
  KeAcquireInStackQueuedSpinLock(&Lock, &Handle);
  U8 Held = KeGetCurrentIrql();
  Shared += 7;
  KeReleaseInStackQueuedSpinLock(&Handle);
  U8 After = KeGetCurrentIrql();
  DbgPrint("qspin before %u held %u after %u value %llu", Before, Held, After,
           Shared);
  return 0;
}
