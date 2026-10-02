//===- windows_export_process.c - Original GetProcAddress oracle --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsExportFixture.h"
__declspec(dllimport) DWORD TopProbe(void);
__declspec(dllimport) DWORD TopOrdinal(void);
__declspec(dllimport) DWORD TopData;
__declspec(dllimport) DWORD TopPID(void);
__declspec(dllexport) DWORD ExeProbe(void) { return Seed; }
static char LongName[MaxLookupName];
static void missing(void *Module, const char *Name) {
  SetLastError(LastErrorSeed);
  CHECK(!GetProcAddress(Module, Name));
  CHECK(GetLastError() == ProcedureMissing);
}
DWORD entry(void) {
  WCHAR *Command = GetCommandLineW();
  DWORD Length = 0;
  while (Command[Length])
    ++Length;
  if (Length && Command[Length - 1] == CommandQuote[0])
    --Length;
  CHECK(Length);
  const WCHAR Mode = Command[Length - 1];
  void *Leaf = GetModuleHandleW(LeafName);
  void *Bridge = GetModuleHandleW(BridgeName);
  void *Top = GetModuleHandleW(TopModuleName);
  CHECK(Leaf && Bridge && Top);
  CHECK(TopProbe() == Seed && TopOrdinal() == Seed + 1 && TopData == Seed);
  CHECK(TopPID() == GetCurrentProcessId());
  void *Code = lookup(Leaf, ProbeName);
  CHECK(lookup(Leaf, AliasName) == Code);
  CHECK(lookup(Top, TopName) == Code && lookup(Bridge, DotName) == Code);
  CHECK(lookup(Top, TopPIDName) == (void *)&GetCurrentProcessId);
  CHECK(lookup(Top, TopDataName) == &TopData);
  CHECK(lookup(Leaf, ValueName) == &TopData);
  CHECK(((DWORD (*)(void))lookup(Top, TopOrdinalName))() == Seed + 1);
  CHECK(((DWORD (*)(void))lookup(Leaf, (const char *)OrdinalOnly))() ==
        Seed + 1);
  CHECK(((DWORD (*)(void))Code)() == Seed);
  CHECK(lookup(GetModuleHandleW(0), OwnName) == (void *)&ExeProbe);
  missing(Leaf, MissingName);
  missing(Leaf, WrongCaseName);
  missing(Leaf, EmptyName);
  missing(Leaf, (const char *)OrdinalHole);
  missing(Leaf, (const char *)OrdinalAbsent);
  missing(Leaf, 0);
  if (Mode == UnusedArgument[0])
    GetProcAddress(Bridge, UnusedName);
  else if (Mode == CycleArgument[0])
    GetProcAddress(Bridge, CycleName);
  else if (Mode == BadOrdinalArgument[0])
    GetProcAddress(Bridge, BadOrdinalName);
  else if (Mode == InvalidNameArgument[0])
    GetProcAddress(Leaf, (const char *)(ULONG_PTR)-1);
  else if (Mode == LongNameArgument[0]) {
    for (DWORD I = 0; I < sizeof(LongName); ++I)
      LongName[I] = LongNameByte[0];
    GetProcAddress(Leaf, LongName);
  } else if (Mode == ChangedEATArgument[0] ||
             Mode == ChangedHeaderArgument[0] ||
             Mode == ChangedDirectoryArgument[0] ||
             Mode == UnreadableArgument[0]) {
    unsigned char *Base = Leaf;
    DWORD *PE = (DWORD *)(Base + PEOffset);
    DWORD *Directory = (DWORD *)(Base + *PE + PEExportDirectory);
    DWORD *Table = (DWORD *)(Base + *Directory + ExportFunctions);
    DWORD *Changed = Mode == ChangedEATArgument[0] ? (DWORD *)(Base + *Table)
                     : Mode == ChangedDirectoryArgument[0] ? Directory
                                                           : PE;
    DWORD Old;
    CHECK(VirtualProtect(Changed, sizeof(*Changed), PageReadWrite, &Old));
    if (Mode == UnreadableArgument[0])
      CHECK(VirtualProtect(Changed, sizeof(*Changed), PageNoAccess, &Old));
    else
      ++*Changed;
    // Query through two other images; changes to the final target count too.
    GetProcAddress(Top, TopName);
  } else if (Mode == NormalArgument[0]) {
    output(Message, sizeof(Message) - 1);
    ExitProcess(ExitStatus);
  }
  ExitProcess(FailureStatus);
  return FailureStatus;
}
