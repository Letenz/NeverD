//===- driver_slist.c - Original sequenced list fixture -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Caches fixed-size entries on a lock-free sequenced list the way drivers
/// recycle buffers, without host or WDK dependencies.
///
//===----------------------------------------------------------------------===//

typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct SLIST_ENTRY {
  _Alignas(16) struct SLIST_ENTRY *Next;
} SLIST_ENTRY;
typedef struct {
  _Alignas(16) U64 Alignment;
  U64 Region;
} SLIST_HEADER;
typedef struct {
  SLIST_ENTRY Link;
  U32 Value;
} ITEM;

__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) void *ExAllocatePoolWithTag(int, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
// x64 wdm.h maps InterlockedPushEntrySList and its relatives to these.
__declspec(dllimport) void InitializeSListHead(SLIST_HEADER *);
__declspec(dllimport) SLIST_ENTRY *ExpInterlockedPushEntrySList(SLIST_HEADER *,
                                                                SLIST_ENTRY *);
__declspec(dllimport) SLIST_ENTRY *ExpInterlockedPopEntrySList(SLIST_HEADER *);
__declspec(dllimport) SLIST_ENTRY *ExpInterlockedFlushSList(SLIST_HEADER *);
__declspec(dllimport) U16 ExQueryDepthSList(SLIST_HEADER *);
__declspec(dllimport) SLIST_ENTRY *FirstEntrySList(SLIST_HEADER *);

NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  const U32 Tag = 0x74534c53;
  SLIST_HEADER *Header = ExAllocatePoolWithTag(0, sizeof(SLIST_HEADER), Tag);
  ITEM *Items = ExAllocatePoolWithTag(0, 4 * sizeof(ITEM), Tag);
  if (!Header || !Items)
    return (NTSTATUS)0xc000009aU;
  InitializeSListHead(Header);
  for (U32 I = 0; I < 4; ++I) {
    Items[I].Value = I + 1;
    ExpInterlockedPushEntrySList(Header, &Items[I].Link);
  }
  U32 Depth = ExQueryDepthSList(Header);
  U32 Top = ((ITEM *)FirstEntrySList(Header))->Value;
  U32 Popped = ((ITEM *)ExpInterlockedPopEntrySList(Header))->Value;

  // Detach the remaining entries at once and walk their links.
  U32 Chain = 0, Count = 0;
  for (SLIST_ENTRY *Entry = ExpInterlockedFlushSList(Header); Entry;
       Entry = Entry->Next) {
    Chain = Chain * 10 + ((ITEM *)Entry)->Value;
    ++Count;
  }
  U32 Empty =
      !ExpInterlockedPopEntrySList(Header) && !ExQueryDepthSList(Header);
  DbgPrint("slist depth %u top %u popped %u chain %u count %u empty %u", Depth,
           Top, Popped, Chain, Count, Empty);
  ExFreePoolWithTag(Items, Tag);
  ExFreePoolWithTag(Header, Tag);
  return 0;
}
