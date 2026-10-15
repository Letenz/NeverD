//===- driver_cstrings.c - Original C runtime string fixture --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercises the C runtime string helpers the kernel exports and drivers
/// import, without host or WDK dependencies. Each result is reduced to an
/// integer so the test can assert a single DbgPrint line.
///
//===----------------------------------------------------------------------===//

typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;

__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) U64 strlen(const char *);
__declspec(dllimport) U64 wcslen(const U16 *);
__declspec(dllimport) U64 strnlen(const char *, U64);
__declspec(dllimport) U64 wcsnlen(const U16 *, U64);
__declspec(dllimport) int strcmp(const char *, const char *);
__declspec(dllimport) int wcscmp(const U16 *, const U16 *);
__declspec(dllimport) int strncmp(const char *, const char *, U64);
__declspec(dllimport) int wcsncmp(const U16 *, const U16 *, U64);
__declspec(dllimport) int _stricmp(const char *, const char *);
__declspec(dllimport) int _wcsicmp(const U16 *, const U16 *);
__declspec(dllimport) int _strnicmp(const char *, const char *, U64);
__declspec(dllimport) int _wcsnicmp(const U16 *, const U16 *, U64);
__declspec(dllimport) char *strchr(const char *, int);
__declspec(dllimport) U16 *wcschr(const U16 *, U16);
__declspec(dllimport) char *strrchr(const char *, int);
__declspec(dllimport) U16 *wcsrchr(const U16 *, U16);
__declspec(dllimport) char *strstr(const char *, const char *);
__declspec(dllimport) U16 *wcsstr(const U16 *, const U16 *);
__declspec(dllimport) U64 strspn(const char *, const char *);
__declspec(dllimport) U64 wcsspn(const U16 *, const U16 *);
__declspec(dllimport) U64 wcscspn(const U16 *, const U16 *);

// The sign of a comparison as -1, 0 or 1, so the printed value stays stable.
static int sign(int Value) { return (Value > 0) - (Value < 0); }

// The index of a match within its string, or -1 when the search misses.
static long long offset(const void *Match, const void *Base, U64 Width) {
  if (!Match)
    return -1;
  return ((const char *)Match - (const char *)Base) / (long long)Width;
}

static const char Narrow[] = "hello world";
static const char Abc[] = "abc", Abd[] = "abd", AbcUpper[] = "ABC";
static const char AbcLowerTail[] = "abcZZZ", AbcUpperTail[] = "ABCxyz";
static const char World[] = "world", Empty[] = "", Spanner[] = "aabbcd";
static const char SpanSet[] = "ab";
static const U16 Wide[] = {'h', 'e', 'l', 'l', 'o', 0};
static const U16 WideUpper[] = {'H', 'E', 'L', 'L', 'O', 0};
static const U16 WideFoldTail[] = {'H', 'E', 'z', 0};
static const U16 WideSubNeedle[] = {'l', 'l', 0};
static const U16 WideSpanSet[] = {'h', 'e', 0};
static const U16 WideCspnSet[] = {'l', 0};

NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;

  U64 NarrowLen = strlen(Narrow);
  U64 WideLen = wcslen(Wide);
  U64 BoundedNarrow = strnlen(Narrow, 3);
  U64 BoundedWide = wcsnlen(Wide, 10);

  int Less = sign(strcmp(Abc, Abd));
  int WideEqual = wcscmp(Wide, Wide);
  int CountedEqual = strncmp(Abc, Abd, 2);
  int WideRaw = wcsncmp(Wide, Wide, 5);
  int FoldNarrow = _stricmp(AbcUpper, Abc);
  int FoldWide = _wcsicmp(Wide, WideUpper);
  int FoldCountedNarrow = _strnicmp(AbcUpperTail, AbcLowerTail, 3);
  int FoldCountedWide = _wcsnicmp(Wide, WideFoldTail, 2);

  long long FirstL = offset(strchr(Narrow, 'l'), Narrow, 1);
  long long WideFirstL = offset(wcschr(Wide, 'l'), Wide, 2);
  long long LastL = offset(strrchr(Narrow, 'l'), Narrow, 1);
  long long WideLastL = offset(wcsrchr(Wide, 'l'), Wide, 2);
  long long Missing = offset(strchr(Narrow, 'z'), Narrow, 1);

  long long WorldAt = offset(strstr(Narrow, World), Narrow, 1);
  long long EmptyAt = offset(strstr(Narrow, Empty), Narrow, 1);
  long long WideSub = offset(wcsstr(Wide, WideSubNeedle), Wide, 2);

  U64 Span = strspn(Spanner, SpanSet);
  U64 WideSpan = wcsspn(Wide, WideSpanSet);
  U64 WideComplement = wcscspn(Wide, WideCspnSet);

  DbgPrint("cstrings %llu %llu %llu %llu | %d %d %d %d %d %d %d %d | "
           "%lld %lld %lld %lld %lld | %lld %lld %lld | %llu %llu %llu",
           NarrowLen, WideLen, BoundedNarrow, BoundedWide, Less, WideEqual,
           CountedEqual, WideRaw, FoldNarrow, FoldWide, FoldCountedNarrow,
           FoldCountedWide, FirstL, WideFirstL, LastL, WideLastL, Missing,
           WorldAt, EmptyAt, WideSub, Span, WideSpan, WideComplement);
  return 0;
}
