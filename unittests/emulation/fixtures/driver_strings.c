//===- driver_strings.c - Original counted-string runtime fixture ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Builds, extends, compares, formats and parses counted strings through the
/// kernel runtime helpers that ordinary WDM drivers import, without host or WDK
/// dependencies.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef int NTSTATUS;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  U16 *Buffer;
} UNICODE_STRING;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  char *Buffer;
} STRING;

__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) void RtlInitAnsiString(STRING *, const char *);
__declspec(dllimport) void RtlInitString(STRING *, const char *);
__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);
__declspec(dllimport) NTSTATUS RtlAppendUnicodeToString(UNICODE_STRING *,
                                                        const U16 *);
__declspec(dllimport) NTSTATUS
RtlAppendUnicodeStringToString(UNICODE_STRING *, const UNICODE_STRING *);
__declspec(dllimport) U8 RtlPrefixUnicodeString(const UNICODE_STRING *,
                                                const UNICODE_STRING *, U8);
__declspec(dllimport) NTSTATUS RtlIntegerToUnicodeString(U32, U32,
                                                         UNICODE_STRING *);
__declspec(dllimport) NTSTATUS RtlUnicodeStringToInteger(const UNICODE_STRING *,
                                                         U32, U32 *);

NTSTATUS DriverEntry(void *Driver, const UNICODE_STRING *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  STRING Name, Empty;
  RtlInitAnsiString(&Name, "driver");
  RtlInitString(&Empty, "");
  if (Name.Length != 6 || Name.MaximumLength != 7 || Empty.Length ||
      Empty.MaximumLength != 1)
    return (NTSTATUS)0xc000000dU;

  // Build a device path from fixed text and a formatted instance number.
  U16 Storage[32];
  UNICODE_STRING Path = {0, sizeof(Storage), 0, Storage};
  U16 Digits[8];
  UNICODE_STRING Number = {0, sizeof(Digits), 0, Digits};
  if (RtlAppendUnicodeToString(&Path, (const U16 *)L"\\Device\\") ||
      RtlAppendUnicodeToString(&Path, (const U16 *)L"Sample") ||
      RtlIntegerToUnicodeString(42, 10, &Number) ||
      RtlAppendUnicodeStringToString(&Path, &Number) ||
      Path.Buffer[Path.Length / 2])
    return (NTSTATUS)0xc000000dU;

  UNICODE_STRING Prefix, Text;
  RtlInitUnicodeString(&Prefix, (const U16 *)L"\\Device\\");
  RtlInitUnicodeString(&Text, (const U16 *)L"  -0x2A");
  U32 Instance = 0, Signed = 0;
  if (!RtlPrefixUnicodeString(&Prefix, &Path, 0) ||
      RtlPrefixUnicodeString(&Path, &Prefix, 0) ||
      RtlUnicodeStringToInteger(&Number, 0, &Instance) ||
      RtlUnicodeStringToInteger(&Text, 0, &Signed))
    return (NTSTATUS)0xc000000dU;

  // A short destination reports each capacity status.
  U16 Small[2];
  UNICODE_STRING Tight = {0, sizeof(Small), 0, Small};
  NTSTATUS TooSmall = RtlAppendUnicodeStringToString(&Tight, &Path);
  NTSTATUS Overflow = RtlIntegerToUnicodeString(123456, 16, &Tight);

  DbgPrint("strings %wZ %u %d %.*s %u", &Path, Instance, (int)Signed,
           (int)Name.Length, Name.Buffer, Path.Length);
  DbgPrint("status %08x %08x %u", (U32)TooSmall, (U32)Overflow, Tight.Length);
  return 0;
}
