//===- driver_cstring_writes.c - Original string writer fixture -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Calls imported narrow/wide copy and append routines on concrete buffers.
///
//===----------------------------------------------------------------------===//

typedef unsigned short U16;
typedef unsigned long long U64;
typedef int NTSTATUS;
__declspec(dllimport) char *strcpy(char *, const char *);
__declspec(dllimport) char *strncpy(char *, const char *, U64);
__declspec(dllimport) char *strcat(char *, const char *);
__declspec(dllimport) char *strncat(char *, const char *, U64);
__declspec(dllimport) U16 *wcscpy(U16 *, const U16 *);
__declspec(dllimport) U16 *wcsncpy(U16 *, const U16 *, U64);
__declspec(dllimport) U16 *wcscat(U16 *, const U16 *);
__declspec(dllimport) U16 *wcsncat(U16 *, const U16 *, U64);
static const char *volatile Source = "AB";
static const U16 *volatile WideSource = (const U16 *)L"XY";

NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  char Narrow[16];
  U16 Wide[16];
  for (unsigned I = 0; I < 16; ++I) {
    Narrow[I] = '!';
    Wide[I] = '!';
  }
  if (strcpy(Narrow, Source) != Narrow || Narrow[0] != 'A' || Narrow[2] != 0 ||
      Narrow[3] != '!')
    return (NTSTATUS)0xc000000d;
  if (strcat(Narrow, Source) != Narrow || Narrow[3] != 'B' || Narrow[4] != 0)
    return (NTSTATUS)0xc000000d;
  if (strncat(Narrow, Source, 1) != Narrow || Narrow[4] != 'A' ||
      Narrow[5] != 0 || Narrow[6] != '!')
    return (NTSTATUS)0xc000000d;
  if (strncpy(Narrow, Source, 1) != Narrow || Narrow[1] != 'B')
    return (NTSTATUS)0xc000000d;
  if (strncpy(Narrow, Source, 7) != Narrow || Narrow[2] != 0 ||
      Narrow[6] != 0 || Narrow[7] != '!')
    return (NTSTATUS)0xc000000d;
  if (wcscpy(Wide, WideSource) != Wide || Wide[0] != 'X' || Wide[2] != 0 ||
      Wide[3] != '!')
    return (NTSTATUS)0xc000000d;
  if (wcscat(Wide, WideSource) != Wide || Wide[3] != 'Y' || Wide[4] != 0)
    return (NTSTATUS)0xc000000d;
  if (wcsncat(Wide, WideSource, 1) != Wide || Wide[4] != 'X' || Wide[5] != 0 ||
      Wide[6] != '!')
    return (NTSTATUS)0xc000000d;
  if (wcsncpy(Wide, WideSource, 1) != Wide || Wide[1] != 'Y')
    return (NTSTATUS)0xc000000d;
  if (wcsncpy(Wide, WideSource, 7) != Wide || Wide[2] != 0 || Wide[6] != 0 ||
      Wide[7] != '!')
    return (NTSTATUS)0xc000000d;
  if (strncpy((char *)1, (const char *)2, 0) != (char *)1 ||
      strncat((char *)1, (const char *)2, 0) != (char *)1 ||
      wcsncpy((U16 *)1, (const U16 *)2, 0) != (U16 *)1 ||
      wcsncat(Wide, (const U16 *)2, 0) != Wide)
    return (NTSTATUS)0xc000000d;
  return 0;
}
