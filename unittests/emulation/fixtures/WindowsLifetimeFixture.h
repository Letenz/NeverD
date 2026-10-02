//===- WindowsLifetimeFixture.h - Independent startup observation -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_WINDOWS_LIFETIME_FIXTURE_H
#define NEVERD_WINDOWS_LIFETIME_FIXTURE_H
typedef unsigned int DWORD;
typedef unsigned long long ULONG_PTR;
typedef unsigned short WCHAR;
#define NEVERD_LIFETIME_VALUE(Name, Value) enum { Name = Value };
#include "WindowsLifetimeCases.def"
#undef NEVERD_LIFETIME_VALUE
__declspec(dllimport) void ExitProcess(DWORD);
__declspec(dllimport) const WCHAR *GetCommandLineW(void);
__declspec(dllimport) void *GetStdHandle(DWORD);
__declspec(dllimport) int WriteFile(void *, const void *, DWORD, DWORD *,
                                    void *);
static void check(int OK, DWORD Line) {
  if (!OK) {
    DWORD Written;
    WriteFile(GetStdHandle(StderrSelector), &Line, sizeof(Line), &Written, 0);
    ExitProcess(FailureStatus);
  }
}
#define CHECK(Condition) check(!!(Condition), __LINE__)
static DWORD mode(void) {
  const WCHAR *P = GetCommandLineW();
  for (; *P; ++P)
    if (*P == ModePrefix)
      return P[1];
  return NormalMode;
}
static void trace(char Role, char Kind, DWORD Reason, void *Reserved) {
  const char Bytes[] = {Role, Kind, ZeroDigit + (char)Reason,
                        ZeroDigit + (Reserved != 0), LineEnd};
  DWORD Written;
  CHECK(WriteFile(GetStdHandle(StdoutSelector), Bytes, sizeof(Bytes), &Written,
                  0));
  CHECK(Written == sizeof(Bytes));
}
static DWORD Sentinel = Seed;
__declspec(thread) DWORD ThreadValue = Seed;
__declspec(thread) DWORD *ThreadPointer = &Sentinel;
__declspec(thread) unsigned char ThreadZero[ZeroTail];
__declspec(allocate(".tls")) char TLSStart;
__declspec(allocate(".tls$ZZZ")) char TLSEnd;
DWORD _tls_index;
typedef void (*TLSCallback)(void *, DWORD, void *);
static void tls(void *, DWORD, void *);
static TLSCallback Callbacks[] = {tls, 0};
__declspec(allocate(".rdata")) const struct {
  const void *Start, *End;
  DWORD *Index;
  const TLSCallback *Callbacks;
  DWORD ZeroFill, Characteristics;
} _tls_used = {&TLSStart, &TLSEnd, &_tls_index, Callbacks, 0, 0};
static void checkTLS(DWORD Reason) {
  CHECK(ThreadPointer == &Sentinel && *ThreadPointer == Seed);
  if (Reason == AttachReason) {
    CHECK(ThreadValue == Seed);
    for (DWORD I = 0; I < ZeroTail; ++I)
      CHECK(!ThreadZero[I]);
  }
  ++ThreadValue;
}
#endif
