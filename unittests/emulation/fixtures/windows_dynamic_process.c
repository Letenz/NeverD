//===- windows_dynamic_process.c - Original load/unload observations -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsDynamicFixture.h"
#ifdef NEVERD_DYNAMIC_STATIC
__declspec(dllimport) DWORD Probe(void);
#endif
DWORD entry(void) {
#ifdef NEVERD_DYNAMIC_STATIC
  CHECK(Probe() == Seed + 1);
  void *Static = GetModuleHandleW(MiddleName);
  CHECK(Static);
  observation(StaticTag, FreeLibrary(Static));
  observation(MiddleTag, GetModuleHandleW(MiddleName) != 0);
  observation(LeafTag, GetModuleHandleW(LeafName) != 0);
  ExitProcess(ExitStatus);
#else
  const DWORD Mode = mode();
  CHECK(!GetModuleHandleW(MiddleName));
  if (Mode == ErrorsMode) {
    SetLastError(LastErrorSeed);
    observation(InvalidTag, FreeLibrary(0));
    observation(ErrorTag, GetLastError());
    CHECK(!LoadLibraryA(MissingFile));
    observation(ErrorTag, GetLastError());
    ExitProcess(ExitStatus);
  }
  void *Top = 0, *Leaf = 0;
  if (Mode == ForwardedMode || Mode == RepeatedForwardMode ||
      Mode == MissingForwardMode) {
    Top = LoadLibraryW(TopName);
    CHECK(Top && !GetModuleHandleW(MiddleName));
    ProbeFunction F = (ProbeFunction)GetProcAddress(
        Top, Mode == MissingForwardMode ? MissingForwardName : ForwardName);
    if (Mode == MissingForwardMode) {
      observation(MissingTag, F != 0);
      observation(ErrorTag, GetLastError());
      observation(MiddleTag, GetModuleHandleW(MiddleName) != 0);
      CHECK(FreeLibrary(Top));
      observation(LeafTag, GetModuleHandleW(LeafName) != 0);
      ExitProcess(ExitStatus);
    }
    if (Mode == RepeatedForwardMode)
      CHECK(GetProcAddress(Top, ForwardName) == (void *)F);
    CHECK(F && F() == Seed + 1);
  }
  if (Mode == SharedMode) {
    Leaf = LoadLibraryW(LeafName);
    CHECK(Leaf);
  }
  SetLastError(LastErrorSeed);
  void *Middle = (Mode == ForwardedMode || Mode == RepeatedForwardMode)
                     ? GetModuleHandleW(MiddleName)
                     : LoadLibraryW(MiddleName);
  observation(LoadedTag, Middle != 0);
  observation(ErrorTag, GetLastError());
  if (Mode == FailedMiddleMode || Mode == LeafRole ||
      Mode == NestedFailureMode) {
    CHECK(!Middle);
    observation(MiddleTag, GetModuleHandleW(MiddleName) != 0);
    observation(LeafTag, GetModuleHandleW(LeafName) != 0);
    observation(TopTag, GetModuleHandleW(TopName) != 0);
    ExitProcess(ExitStatus);
  }
  CHECK(Middle);
  probe(Middle, Seed + 1);
  if (Mode == ProcessExitMode)
    ExitProcess(ExitStatus);
  if (Mode == ReferencesMode) {
    void *Again = LoadLibraryA(MiddleFile);
    CHECK(Again == Middle);
    CHECK(FreeLibrary(Again));
    CHECK(GetModuleHandleW(MiddleName) == Middle);
    probe(Middle, Seed + 1);
  }
  if (Mode == BorrowedMode)
    Middle = GetModuleHandleW(MiddleName);
  SetLastError(LastErrorSeed);
  observation(FreeTag,
              FreeLibrary((Mode == ForwardedMode || Mode == RepeatedForwardMode)
                              ? Top
                              : Middle));
  observation(ErrorTag, GetLastError());
  observation(MiddleTag, GetModuleHandleW(MiddleName) != 0);
  observation(LeafTag, GetModuleHandleW(LeafName) != 0);
  if (Leaf) {
    CHECK(GetModuleHandleW(LeafName) == Leaf);
    CHECK(FreeLibrary(Leaf));
    CHECK(!GetModuleHandleW(LeafName));
  }
  if (Mode == ReferencesMode) {
    Middle = LoadLibraryA(MiddleFile);
    CHECK(Middle);
    probe(Middle, Seed + 1);
    CHECK(FreeLibrary(Middle));
    CHECK(!GetModuleHandleW(MiddleName) && !GetModuleHandleW(LeafName));
  }
  ExitProcess(ExitStatus);
#endif
}
