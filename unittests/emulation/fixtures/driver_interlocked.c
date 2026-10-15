//===- driver_interlocked.c - Original interlocked queue fixture ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shares a work queue, a free list and counters under one spin lock the way
/// drivers hand work between dispatch and interrupt paths, without host or
/// WDK dependencies.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef long long I64;
typedef int NTSTATUS;
typedef U64 KSPIN_LOCK;
typedef struct LIST_ENTRY {
  struct LIST_ENTRY *Flink, *Blink;
} LIST_ENTRY;
typedef struct SINGLE_LIST_ENTRY {
  struct SINGLE_LIST_ENTRY *Next;
} SINGLE_LIST_ENTRY;
typedef struct {
  LIST_ENTRY Link;
  SINGLE_LIST_ENTRY Free;
  U32 Value;
} ITEM;
typedef struct {
  KSPIN_LOCK Lock;
  LIST_ENTRY Queue;
  SINGLE_LIST_ENTRY FreeList;
  U32 Pending;
  I64 Bytes;
} QUEUE;

#define ITEM_FROM_LINK(Entry) ((ITEM *)(Entry))
#define ITEM_FROM_FREE(Entry)                                                  \
  ((ITEM *)((U8 *)(Entry) - __builtin_offsetof(ITEM, Free)))

__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) void *ExAllocatePoolWithTag(int, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void KeInitializeSpinLock(KSPIN_LOCK *);
__declspec(dllimport) LIST_ENTRY *
ExInterlockedInsertHeadList(LIST_ENTRY *, LIST_ENTRY *, KSPIN_LOCK *);
__declspec(dllimport) LIST_ENTRY *
ExInterlockedInsertTailList(LIST_ENTRY *, LIST_ENTRY *, KSPIN_LOCK *);
__declspec(dllimport) LIST_ENTRY *ExInterlockedRemoveHeadList(LIST_ENTRY *,
                                                              KSPIN_LOCK *);
__declspec(dllimport) SINGLE_LIST_ENTRY *
ExInterlockedPushEntryList(SINGLE_LIST_ENTRY *, SINGLE_LIST_ENTRY *,
                           KSPIN_LOCK *);
__declspec(dllimport) SINGLE_LIST_ENTRY *
ExInterlockedPopEntryList(SINGLE_LIST_ENTRY *, KSPIN_LOCK *);
__declspec(dllimport) U32 ExInterlockedAddUlong(U32 *, U32, KSPIN_LOCK *);
__declspec(dllimport) I64 ExInterlockedAddLargeInteger(I64 *, I64,
                                                       KSPIN_LOCK *);

NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  const U32 Tag = 0x51756554;
  QUEUE *State = ExAllocatePoolWithTag(0, sizeof(QUEUE), Tag);
  ITEM *Items = ExAllocatePoolWithTag(0, 3 * sizeof(ITEM), Tag);
  if (!State || !Items)
    return (NTSTATUS)0xc000009aU;
  KeInitializeSpinLock(&State->Lock);
  State->Queue.Flink = State->Queue.Blink = &State->Queue;
  State->FreeList.Next = 0;
  State->Pending = 0;
  State->Bytes = 0;
  for (U32 I = 0; I < 3; ++I)
    Items[I].Value = 10 * (I + 1);

  // Queue 10 and 20 at the tail, then 30 at the head.
  U32 Empty =
      !ExInterlockedInsertTailList(&State->Queue, &Items[0].Link, &State->Lock);
  U32 OldLast = ExInterlockedInsertTailList(&State->Queue, &Items[1].Link,
                                            &State->Lock) == &Items[0].Link;
  U32 OldFirst = ExInterlockedInsertHeadList(&State->Queue, &Items[2].Link,
                                             &State->Lock) == &Items[0].Link;
  for (U32 I = 0; I < 3; ++I) {
    ExInterlockedAddUlong(&State->Pending, 1, &State->Lock);
    ExInterlockedAddLargeInteger(&State->Bytes, Items[I].Value, &State->Lock);
  }

  // Drain the queue in order and recycle each item onto the free list.
  U32 Order[3] = {0, 0, 0}, Count = 0;
  LIST_ENTRY *Entry;
  while ((Entry = ExInterlockedRemoveHeadList(&State->Queue, &State->Lock))) {
    ITEM *Item = ITEM_FROM_LINK(Entry);
    if (Count < 3)
      Order[Count] = Item->Value;
    ++Count;
    ExInterlockedAddUlong(&State->Pending, (U32)-1, &State->Lock);
    ExInterlockedPushEntryList(&State->FreeList, &Item->Free, &State->Lock);
  }
  U32 Reused[3] = {0, 0, 0}, Free = 0;
  SINGLE_LIST_ENTRY *Next;
  while ((Next = ExInterlockedPopEntryList(&State->FreeList, &State->Lock))) {
    if (Free < 3)
      Reused[Free] = ITEM_FROM_FREE(Next)->Value;
    ++Free;
  }
  DbgPrint("queue %u %u %u order %u %u %u free %u %u %u pending %u bytes %lld",
           Empty, OldLast, OldFirst, Order[0], Order[1], Order[2], Reused[0],
           Reused[1], Reused[2], State->Pending, State->Bytes);
  ExFreePoolWithTag(Items, Tag);
  ExFreePoolWithTag(State, Tag);
  return Count == 3 && Free == 3 ? 0 : (NTSTATUS)0xc0000001U;
}
