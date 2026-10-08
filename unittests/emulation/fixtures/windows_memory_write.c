//===- windows_memory_write.c - Original Windows page-write observations --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Freestanding calls to the real Windows API; no emulator implementation or
// Windows SDK headers are used to determine these observations.
typedef unsigned int DWORD;
typedef unsigned long long ULONG_PTR;
typedef void *HANDLE;
#define NEVERD_MEMORY_WRITE_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_MEMORY_WRITE_WIDE(Name, Value)                                  \
  static const ULONG_PTR Name = Value;
#define NEVERD_MEMORY_WRITE_PROTECTION(Name, Value) enum { Name = Value };
#include "WindowsMemoryWriteCases.def"
#undef NEVERD_MEMORY_WRITE_PROTECTION
#undef NEVERD_MEMORY_WRITE_VALUE
#undef NEVERD_MEMORY_WRITE_WIDE

typedef struct {
  void *Base, *AllocationBase;
  DWORD AllocationProtection;
  unsigned short Partition;
  ULONG_PTR Size;
  DWORD State, Protection, Type;
} MemoryInfo;
typedef struct {
#define NEVERD_MEMORY_WRITE_FIELD(Name) ULONG_PTR Name;
#include "WindowsMemoryWriteCases.def"
#undef NEVERD_MEMORY_WRITE_FIELD
} Observation;
#define NEVERD_MEMORY_WRITE_ABI(Type, Size, Text)                              \
  _Static_assert(sizeof(Type) == Size, Text);
#include "WindowsMemoryWriteCases.def"
#undef NEVERD_MEMORY_WRITE_ABI

__declspec(dllimport) void ExitProcess(DWORD);
__declspec(dllimport) HANDLE GetCurrentProcess(void);
__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) void SetLastError(DWORD);
__declspec(dllimport) int WriteFile(HANDLE, const void *, DWORD, DWORD *,
                                    void *);
__declspec(dllimport) void *VirtualAlloc(void *, ULONG_PTR, DWORD, DWORD);
__declspec(dllimport) int VirtualFree(void *, ULONG_PTR, DWORD);
__declspec(dllimport) int VirtualProtect(void *, ULONG_PTR, DWORD, DWORD *);
__declspec(dllimport) ULONG_PTR VirtualQuery(const void *, MemoryInfo *,
                                             ULONG_PTR);
__declspec(dllimport) int WriteProcessMemory(HANDLE, void *, const void *,
                                             ULONG_PTR, ULONG_PTR *);

static void require(int OK) {
  if (!OK)
    ExitProcess(FailureStatus);
}
static DWORD protection(void *Address) {
  MemoryInfo Info;
  require(VirtualQuery(Address, &Info, sizeof(Info)) == sizeof(Info));
  return Info.Protection;
}
void entry(void) {
  static const DWORD Protections[] = {
#define NEVERD_MEMORY_WRITE_PROTECTION(Name, Value) Name,
#include "WindowsMemoryWriteCases.def"
#undef NEVERD_MEMORY_WRITE_PROTECTION
  };
  static const unsigned char Bytes[WriteSize] = {UpdatedFirst, UpdatedSecond};
  unsigned char *Base =
      VirtualAlloc(0, 2 * PageSize, MemCommitReserve, ReadWrite);
  require(Base != 0);
  const unsigned Count = sizeof(Protections) / sizeof(Protections[0]);
  for (unsigned First = 0; First < Count; ++First) {
    for (unsigned Second = 0; Second < Count; ++Second) {
      DWORD Old;
      require(VirtualProtect(Base, 2 * PageSize, ReadWrite, &Old));
      Base[PageSize - 1] = InitialFirst;
      Base[PageSize] = InitialSecond;
      require(VirtualProtect(Base, PageSize, Protections[First], &Old));
      require(
          VirtualProtect(Base + PageSize, PageSize, Protections[Second], &Old));
      ULONG_PTR Written = WrittenSeed;
      SetLastError(LastErrorSeed);
      const int Result =
          WriteProcessMemory(GetCurrentProcess(), Base + PageSize - 1, Bytes,
                             sizeof(Bytes), &Written);
      Observation Record;
      Record.First = First;
      Record.Second = Second;
      Record.Result = Result != 0;
      Record.Error = GetLastError();
      Record.Written = Written;
      Record.AfterFirst = protection(Base);
      Record.AfterSecond = protection(Base + PageSize);
      require(VirtualProtect(Base, 2 * PageSize, ReadWrite, &Old));
      Record.ByteFirst = Base[PageSize - 1];
      Record.ByteSecond = Base[PageSize];
      DWORD Output;
      require(WriteFile(GetStdHandle(StdoutSelector), &Record, sizeof(Record),
                        &Output, 0));
      require(Output == sizeof(Record));
    }
  }
  require(VirtualFree(Base, 0, MemRelease));
  ExitProcess(CompletionStatus);
}
