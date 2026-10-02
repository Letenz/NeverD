//===- WindowsExportFixture.h - Independent export fixture ABI -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_WINDOWS_EXPORT_FIXTURE_H
#define NEVERD_WINDOWS_EXPORT_FIXTURE_H
typedef unsigned int DWORD;
typedef unsigned long long ULONG_PTR;
typedef unsigned short WCHAR;
#define NEVERD_EXPORT_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_EXPORT_TEXT(Name, Text) static const char Name[] = Text;
#define NEVERD_EXPORT_WIDE(Name, Text) static const WCHAR Name[] = Text;
#include "WindowsExportCases.def"
#undef NEVERD_EXPORT_WIDE
#undef NEVERD_EXPORT_TEXT
#undef NEVERD_EXPORT_VALUE
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) void *GetModuleHandleW(const WCHAR *);
__declspec(dllimport) DWORD GetCurrentProcessId(void);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) void SetLastError(DWORD);
__declspec(dllimport) void *GetStdHandle(DWORD);
__declspec(dllimport) int WriteFile(void *, const void *, DWORD, DWORD *,
                                    void *);
__declspec(dllimport) void ExitProcess(DWORD);
__declspec(dllimport) WCHAR *GetCommandLineW(void);
__declspec(dllimport) int VirtualProtect(void *, ULONG_PTR, DWORD, DWORD *);
static void check(int OK, DWORD Line) {
  if (!OK) {
    DWORD Written;
    WriteFile(GetStdHandle(StderrSelector), &Line, sizeof(Line), &Written, 0);
    ExitProcess(FailureStatus);
  }
}
#define CHECK(Condition) check(!!(Condition), __LINE__)
static void output(const char *Bytes, DWORD Size) {
  DWORD Written;
  CHECK(WriteFile(GetStdHandle(StdoutSelector), Bytes, Size, &Written, 0));
  CHECK(Written == Size);
}
static void *lookup(void *Module, const char *Name) {
  SetLastError(LastErrorSeed);
  void *Address = GetProcAddress(Module, Name);
  CHECK(Address && GetLastError() == LastErrorSeed);
  return Address;
}
#endif
