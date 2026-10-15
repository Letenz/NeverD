//===- driver_lookaside.c - Original lookaside cache fixture --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Public x64 layout and documented WDK inline allocation/free behavior.
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  _Alignas(16) U64 Alignment;
  U64 Region;
} SLIST_HEADER;
typedef struct ENTRY {
  struct ENTRY *Next;
  U64 Value;
  U64 Padding[2];
} ENTRY;
typedef struct {
  SLIST_HEADER Head;
  U16 Depth, MaximumDepth;
  U32 TotalAllocates, AllocateMisses, TotalFrees, FreeMisses;
  U32 Type, Tag, Size;
  void *(*Allocate)(U32, U64, U32);
  void (*Free)(void *);
  U64 Flink, Blink;
  U32 LastTotalAllocates, LastAllocateMisses, Future[2];
} LOOKASIDE;
_Static_assert(sizeof(LOOKASIDE) == 96, "x64 lookaside ABI");
__declspec(dllimport) void ExInitializeNPagedLookasideList(LOOKASIDE *, void *,
                                                           void *, U32, U64,
                                                           U32, U16);
__declspec(dllimport) void
ExInitializePagedLookasideList(LOOKASIDE *, void *, void *, U32, U64, U32, U16);
__declspec(dllimport) void ExDeleteNPagedLookasideList(LOOKASIDE *);
__declspec(dllimport) void ExDeletePagedLookasideList(LOOKASIDE *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePool(void *);
__declspec(dllimport) ENTRY *ExpInterlockedPopEntrySList(SLIST_HEADER *);
__declspec(dllimport) ENTRY *ExpInterlockedPushEntrySList(SLIST_HEADER *,
                                                          ENTRY *);
__declspec(dllimport) U16 ExQueryDepthSList(SLIST_HEADER *);
__declspec(dllimport) unsigned char KfRaiseIrql(unsigned char);
__declspec(dllimport) void KeLowerIrql(unsigned char);
static LOOKASIDE *Lists[3];

static ENTRY *take(LOOKASIDE *L) {
  ++L->TotalAllocates;
  ENTRY *E = ExpInterlockedPopEntrySList(&L->Head);
  if (!E) {
    ++L->AllocateMisses;
    E = L->Allocate(L->Type, L->Size, L->Tag);
  }
  return E;
}
static void put(LOOKASIDE *L, ENTRY *E) {
  ++L->TotalFrees;
  if (ExQueryDepthSList(&L->Head) >= L->Depth) {
    ++L->FreeMisses;
    L->Free(E);
  } else {
    ExpInterlockedPushEntrySList(&L->Head, E);
  }
}
static NTSTATUS exercise(LOOKASIDE *L) {
  ENTRY *Entries[6];
  for (U32 I = 0; I < 6; ++I) {
    Entries[I] = take(L);
    if (!Entries[I])
      return (NTSTATUS)0xc000009a;
    Entries[I]->Value = 100 + I;
  }
  for (U32 I = 0; I < 6; ++I)
    put(L, Entries[I]);
  ENTRY *Hit = take(L);
  if (Hit != Entries[3] || Hit->Value != 103)
    return (NTSTATUS)0xc000000d;
  put(L, Hit);
  if (L->TotalAllocates != 7 || L->AllocateMisses != 6 || L->TotalFrees != 7 ||
      L->FreeMisses != 2 || ExQueryDepthSList(&L->Head) != 4)
    return (NTSTATUS)0xc000000d;
  return 0;
}
static void unload(void *Driver) {
  (void)Driver;
  // Remove the first list while a second list shares its nonpaged registry.
  ExDeleteNPagedLookasideList(Lists[0]);
  ExDeletePagedLookasideList(Lists[1]);
  ExDeleteNPagedLookasideList(Lists[2]);
  for (U32 I = 0; I < 3; ++I)
    ExFreePool(Lists[I]);
}
static void (*volatile Unload)(void *) = unload;
NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)RegistryPath;
  for (U32 I = 0; I < 3; ++I) {
    Lists[I] = ExAllocatePoolWithTag(0, sizeof(LOOKASIDE), 0x74734c4e);
    if (!Lists[I])
      return (NTSTATUS)0xc000009a;
    if (I == 1)
      ExInitializePagedLookasideList(Lists[I], 0, 0, 0, sizeof(ENTRY),
                                     0x74734c4e, 0);
    else
      ExInitializeNPagedLookasideList(Lists[I], 0, 0, I ? 512 : 0,
                                      sizeof(ENTRY), 0x74734c4e, 0);
    unsigned char Old = I == 1 ? 0 : KfRaiseIrql(2);
    NTSTATUS Status = exercise(Lists[I]);
    if (I != 1)
      KeLowerIrql(Old);
    if (Status)
      return Status;
  }
  *(void (**)(void *))((unsigned char *)Driver + 0x68) = Unload;
  return 0;
}
