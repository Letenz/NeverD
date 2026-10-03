//===- windows_heap.c - Original Win32 heap observations -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned short U16;
typedef unsigned char U8;
#define NEVERD_HEAP_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_HEAP_MODE(Name, Value) enum { Name = Value };
#include "WindowsHeapCases.def"
#undef NEVERD_HEAP_MODE
#undef NEVERD_HEAP_VALUE
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) U16 *GetCommandLineW(void);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) U32 GetLastError(void);
__declspec(dllimport) void SetLastError(U32);
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *, U32, U64);
__declspec(dllimport) void *HeapReAlloc(void *, U32, void *, U64);
__declspec(dllimport) int HeapFree(void *, U32, void *);
__declspec(dllimport) U64 HeapSize(void *, U32, const void *);
__declspec(dllimport) U16 *GetEnvironmentStringsW(void);

static void require(int Valid, U32 Site) {
  if (Valid)
    return;
  U32 Written;
  WriteFile(GetStdHandle(StderrSelector), &Site, sizeof(Site), &Written, 0);
  ExitProcess(FailureStatus);
}
static U8 pattern(U64 I) { return (U8)(I ^ PatternSeed); }
static void fill(U8 *P, U64 Size) {
  for (U64 I = 0; I < Size; ++I)
    P[I] = pattern(I);
}
static void prefix(const U8 *P, U64 Size) {
  for (U64 I = 0; I < Size; ++I)
    require(P[I] == pattern(I), 1);
}
static void zeroes(const U8 *P, U64 Begin, U64 End) {
  for (U64 I = Begin; I < End; ++I)
    require(P[I] == 0, 2);
}
static U8 *resize(void *Heap, U8 *P, U64 Size, U32 Flags) {
  SetLastError(LastErrorSeed);
  U8 *Next = HeapReAlloc(Heap, Flags, P, Size);
  require(Next != 0 && (U64)Next % HeapAlignment == 0, 3);
  require(GetLastError() == LastErrorSeed, 4);
  require(HeapSize(Heap, 0, Next) == Size, 5);
  if (Flags & HeapInPlace)
    require(Next == P, 6);
  return Next;
}
static void runResize(void *Heap) {
  U8 *P = HeapAlloc(Heap, 0, SmallSize);
  U8 *Neighbor = HeapAlloc(Heap, 0, PageSize);
  require(P && Neighbor, 7);
  fill(P, SmallSize);
  Neighbor[0] = Poison;
  P = resize(Heap, P, LargeSize, HeapZero);
  prefix(P, SmallSize);
  zeroes(P, SmallSize, LargeSize);
  fill(P, LargeSize);
  P = resize(Heap, P, TinySize, HeapInPlace);
  prefix(P, TinySize);
  P = resize(Heap, P, MediumSize, HeapZero);
  prefix(P, TinySize);
  zeroes(P, TinySize, MediumSize);
  require(Neighbor[0] == Poison, 8);
  require(HeapFree(Heap, 0, P) && HeapFree(Heap, 0, Neighbor), 9);
}
static void runFailure(void *Heap) {
  U8 *P = HeapAlloc(Heap, 0, SmallSize);
  require(P != 0, 10);
  fill(P, SmallSize);
  for (U32 Flags = 0; Flags <= HeapInPlace; Flags += HeapInPlace) {
    SetLastError(LastErrorSeed);
    require(HeapReAlloc(Heap, Flags, P, ~(U64)0) == 0, 11);
    require(GetLastError() == LastErrorSeed, 12);
    require(HeapSize(Heap, 0, P) == SmallSize, 13);
    prefix(P, SmallSize);
  }
  require(HeapFree(Heap, 0, P), 14);
}
static void runZero(void *Heap) {
  U8 *P = HeapAlloc(Heap, HeapZero, 0);
  require(P != 0 && HeapSize(Heap, 0, P) == 0, 15);
  P = resize(Heap, P, SmallSize, HeapZero);
  zeroes(P, 0, SmallSize);
  fill(P, SmallSize);
  P = resize(Heap, P, 0, HeapInPlace);
  P = resize(Heap, P, SmallSize, HeapZero);
  zeroes(P, 0, SmallSize);
  require(HeapFree(Heap, 0, P), 16);
}
static void runInPlace(void *Heap) {
  U8 *P = HeapAlloc(Heap, 0, LargeSize);
  require(P != 0, 17);
  fill(P, TinySize);
  P = resize(Heap, P, TinySize, HeapInPlace | HeapZero);
  prefix(P, TinySize);
  P = resize(Heap, P, TinySize, HeapInPlace);
  prefix(P, TinySize);
  require(HeapFree(Heap, 0, P), 18);
}
static void runReclaim(void *Heap) {
  for (U32 I = 0; I < RepeatCount; ++I) {
    U8 *P = HeapAlloc(Heap, HeapZero, SmallSize);
    U8 *Neighbor = HeapAlloc(Heap, 0, PageSize);
    require(P && Neighbor, 19);
    P[0] = Poison;
    P = resize(Heap, P, LargeSize, HeapZero);
    require(P[0] == Poison && P[LargeSize - 1] == 0, 20);
    P = resize(Heap, P, TinySize, HeapInPlace);
    require(P[0] == Poison, 21);
    require(HeapFree(Heap, 0, P) && HeapFree(Heap, 0, Neighbor), 22);
  }
}
void entry(void) {
  U16 *Command = GetCommandLineW();
  while (*Command && *Command != ModePrefix)
    ++Command;
  void *Heap = GetProcessHeap();
  switch (Command[1]) {
  case ResizeMode:
    runResize(Heap);
    break;
  case FailureMode:
    runFailure(Heap);
    break;
  case ZeroMode:
    runZero(Heap);
    break;
  case InPlaceMode:
    runInPlace(Heap);
    break;
  case ReclaimMode:
    runReclaim(Heap);
    break;
  case SnapshotMode:
    HeapReAlloc(Heap, 0, GetEnvironmentStringsW(), LargeSize);
    ExitProcess(FailureStatus);
    break;
  default:
    ExitProcess(FailureStatus);
  }
  U32 Values[] = {1, GetLastError()};
  U32 Written;
  WriteFile(GetStdHandle(StdoutSelector), Values, sizeof(Values), &Written, 0);
  ExitProcess(ExitStatus);
}
