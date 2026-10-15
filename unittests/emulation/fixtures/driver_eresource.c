//===- driver_eresource.c - Original ERESOURCE concurrency fixture --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// ERESOURCE storage is opaque. Only documented routines observe its state.
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  _Alignas(8) U8 Bytes[104];
} RESOURCE;
typedef struct {
  _Alignas(8) U8 Bytes[24];
} EVENT;
__declspec(dllimport) NTSTATUS ExInitializeResourceLite(RESOURCE *);
__declspec(dllimport) NTSTATUS ExReinitializeResourceLite(RESOURCE *);
__declspec(dllimport) NTSTATUS ExDeleteResourceLite(RESOURCE *);
__declspec(dllimport) U8 ExAcquireResourceExclusiveLite(RESOURCE *, U8);
__declspec(dllimport) U8 ExTryToAcquireResourceExclusiveLite(RESOURCE *);
__declspec(dllimport) U8 ExAcquireResourceSharedLite(RESOURCE *, U8);
__declspec(dllimport) U8 ExAcquireSharedStarveExclusive(RESOURCE *, U8);
__declspec(dllimport) U8 ExAcquireSharedWaitForExclusive(RESOURCE *, U8);
__declspec(dllimport) void ExConvertExclusiveToSharedLite(RESOURCE *);
__declspec(dllimport) void ExReleaseResourceLite(RESOURCE *);
__declspec(dllimport) void ExReleaseResourceForThreadLite(RESOURCE *, U64);
__declspec(dllimport) U8 ExIsResourceAcquiredExclusiveLite(RESOURCE *);
__declspec(dllimport) U32 ExIsResourceAcquiredSharedLite(RESOURCE *);
__declspec(dllimport) U32 ExGetExclusiveWaiterCount(RESOURCE *);
__declspec(dllimport) U32 ExGetSharedWaiterCount(RESOURCE *);
__declspec(dllimport) void ExDisableResourceBoostLite(RESOURCE *);
__declspec(dllimport) void *
ExEnterCriticalRegionAndAcquireResourceExclusive(RESOURCE *);
__declspec(dllimport) void *
ExEnterCriticalRegionAndAcquireResourceShared(RESOURCE *);
__declspec(dllimport) void *
ExEnterCriticalRegionAndAcquireSharedWaitForExclusive(RESOURCE *);
__declspec(dllimport) void ExReleaseResourceAndLeaveCriticalRegion(RESOURCE *);
__declspec(dllimport) void ExSetResourceOwnerPointer(RESOURCE *, void *);
__declspec(dllimport) void ExSetResourceOwnerPointerEx(RESOURCE *, void *, U32);
__declspec(dllimport) void KeEnterCriticalRegion(void);
__declspec(dllimport) void KeLeaveCriticalRegion(void);
__declspec(dllimport) U8 KeAreApcsDisabled(void);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) void *KeGetCurrentThread(void);
__declspec(dllimport) void KeInitializeEvent(EVENT *, U32, U8);
__declspec(dllimport) U32 KeSetEvent(EVENT *, U32, U8);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(void *, U32, U32, U8,
                                                     void *);
__declspec(dllimport) NTSTATUS PsCreateSystemThread(U64 *, U32, void *, U64,
                                                    void *, void (*)(void *),
                                                    void *);
__declspec(dllimport) void PsTerminateSystemThread(NTSTATUS);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(U64, U32, void *, U32,
                                                         void **, void *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) NTSTATUS ZwClose(U64);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePool(void *);
static RESOURCE *R;
static void *Token;
static EVENT Ready[3], Go;
static volatile U32 Failure;
#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)0xc000000d;                                             \
  } while (0)
static void worker(void *Context) {
  U32 I = (U32)(U64)Context;
  // System threads start in a critical region. Keep the resource while parked.
  if (!ExAcquireResourceSharedLite(R, 1) ||
      !ExAcquireSharedWaitForExclusive(R, 0) ||
      ExIsResourceAcquiredSharedLite(R) != 2 ||
      ExIsResourceAcquiredExclusiveLite(R))
    Failure = 1;
  KeSetEvent(&Ready[I], 0, 0);
  KeWaitForSingleObject(&Go, 0, 0, 0, 0);
  ExReleaseResourceForThreadLite(R, (U64)KeGetCurrentThread());
  ExReleaseResourceLite(R);
  if (ExIsResourceAcquiredSharedLite(R))
    Failure = 2;
  PsTerminateSystemThread(0);
}
static void (*volatile Worker)(void *) = worker;
static NTSTATUS concurrent(void) {
  U64 Handles[3];
  void *Threads[3];
  KeInitializeEvent(&Go, 0, 0);
  KeEnterCriticalRegion();
  CHECK(ExAcquireResourceSharedLite(R, 1));
  for (U32 I = 0; I != 3; ++I) {
    KeInitializeEvent(&Ready[I], 1, 0);
    CHECK(!PsCreateSystemThread(&Handles[I], 0x1fffff, 0, 0, 0, Worker,
                                (void *)(U64)I));
    CHECK(
        !ObReferenceObjectByHandle(Handles[I], 0x100000, 0, 0, &Threads[I], 0));
    CHECK(!ZwClose(Handles[I]));
  }
  for (U32 I = 0; I != 3; ++I)
    CHECK(!KeWaitForSingleObject(&Ready[I], 0, 0, 0, 0));
  CHECK(!Failure && !ExTryToAcquireResourceExclusiveLite(R));
  CHECK(!ExAcquireResourceExclusiveLite(R, 0));
  CHECK(ExIsResourceAcquiredSharedLite(R) == 1);
  KeSetEvent(&Go, 0, 0);
  for (U32 I = 0; I != 3; ++I) {
    CHECK(!KeWaitForSingleObject(Threads[I], 0, 0, 0, 0));
    ObfDereferenceObject(Threads[I]);
  }
  CHECK(!Failure);
  ExReleaseResourceLite(R);
  KeLeaveCriticalRegion();
  return 0;
}
static NTSTATUS simple(void) {
  KeEnterCriticalRegion();
  CHECK(ExAcquireResourceExclusiveLite(R, 1));
  CHECK(ExTryToAcquireResourceExclusiveLite(R));
  CHECK(ExAcquireResourceSharedLite(R, 0));
  CHECK(ExIsResourceAcquiredExclusiveLite(R) &&
        ExIsResourceAcquiredSharedLite(R) == 3);
  ExConvertExclusiveToSharedLite(R);
  CHECK(!ExIsResourceAcquiredExclusiveLite(R));
  CHECK(ExAcquireSharedStarveExclusive(R, 1) &&
        ExAcquireSharedWaitForExclusive(R, 1));
  for (U32 I = 5; I; --I) {
    CHECK(ExIsResourceAcquiredSharedLite(R) == I);
    ExReleaseResourceLite(R);
  }
  CHECK(!ExGetSharedWaiterCount(R) && !ExGetExclusiveWaiterCount(R));
  KeLeaveCriticalRegion();
  ExDisableResourceBoostLite(R);
  CHECK(!ExReinitializeResourceLite(R));
  ExEnterCriticalRegionAndAcquireResourceExclusive(R);
  CHECK(KeAreApcsDisabled() && ExIsResourceAcquiredExclusiveLite(R));
  ExReleaseResourceAndLeaveCriticalRegion(R);
  ExEnterCriticalRegionAndAcquireResourceShared(R);
  CHECK(KeAreApcsDisabled() && ExIsResourceAcquiredSharedLite(R) == 1);
  ExReleaseResourceAndLeaveCriticalRegion(R);
  ExEnterCriticalRegionAndAcquireSharedWaitForExclusive(R);
  CHECK(KeAreApcsDisabled() && ExIsResourceAcquiredSharedLite(R) == 1);
  ExReleaseResourceAndLeaveCriticalRegion(R);
  for (U32 I = 0; I != 2; ++I) {
    KeEnterCriticalRegion();
    CHECK(ExAcquireResourceExclusiveLite(R, 1));
    if (I)
      ExSetResourceOwnerPointerEx(R, (void *)((U64)Token | 3), 0);
    else
      ExSetResourceOwnerPointer(R, (void *)((U64)Token | 3));
    ExReleaseResourceForThreadLite(R, (U64)Token | 3);
    KeLeaveCriticalRegion();
  }
  CHECK(!KeAreApcsDisabled() && !KeGetCurrentIrql());
  return 0;
}
static void unload(void *Driver) {
  (void)Driver;
  ExDeleteResourceLite(R);
  ExFreePool(R);
  ExFreePool(Token);
}
static void (*volatile Unload)(void *) = unload;
NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)RegistryPath;
  R = ExAllocatePoolWithTag(0, sizeof(*R), 0x63725345);
  Token = ExAllocatePoolWithTag(0, 8, 0x63725345);
  CHECK(R && Token && !ExInitializeResourceLite(R));
  NTSTATUS Status = simple();
  if (!Status)
    Status = concurrent();
  if (Status)
    return Status;
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  return 0;
}
