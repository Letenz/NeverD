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
__declspec(dllimport) U8 RtlEqualUnicodeString(const UNICODE_STRING *,
                                               const UNICODE_STRING *, U8);
__declspec(dllimport) NTSTATUS RtlAnsiStringToUnicodeString(UNICODE_STRING *,
                                                            const STRING *, U8);
__declspec(dllimport) NTSTATUS
RtlUnicodeStringToAnsiString(STRING *, const UNICODE_STRING *, U8);
__declspec(dllimport) void RtlFreeUnicodeString(UNICODE_STRING *);
__declspec(dllimport) void RtlFreeAnsiString(STRING *);
__declspec(dllimport) NTSTATUS RtlInitUnicodeStringEx(UNICODE_STRING *,
                                                      const U16 *);
__declspec(dllimport) void RtlCopyString(STRING *, const STRING *);
__declspec(dllimport) U8 RtlEqualString(const STRING *, const STRING *, U8);
__declspec(dllimport) int RtlCompareString(const STRING *, const STRING *, U8);

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

  // Round-trip the path through allocated copies, then release both.
  STRING Narrow;
  UNICODE_STRING Wide;
  if (RtlUnicodeStringToAnsiString(&Narrow, &Path, 1) ||
      Narrow.Length != Path.Length / 2 || Narrow.Buffer[Narrow.Length] ||
      RtlAnsiStringToUnicodeString(&Wide, &Narrow, 1) ||
      !RtlEqualUnicodeString(&Wide, &Path, 0))
    return (NTSTATUS)0xc000000dU;
  RtlFreeAnsiString(&Narrow);
  RtlFreeUnicodeString(&Wide);
  if (Narrow.Buffer || Narrow.Length || Wide.Buffer || Wide.Length)
    return (NTSTATUS)0xc000000dU;
  // A short ANSI buffer keeps the terminated prefix that fits.
  char Head[4];
  STRING Short = {0, sizeof(Head), 0, Head};
  NTSTATUS Truncated = RtlUnicodeStringToAnsiString(&Short, &Path, 0);
  DbgPrint("convert %08x %u %s", (U32)Truncated, Short.Length, Head);

  // Bounded initialization, terminator-free byte copy and byte comparison.
  UNICODE_STRING Bounded;
  char Bytes[8] = {'!', '!', '!', '!', '!', '!', '!', '!'};
  STRING Copied = {0, sizeof(Bytes), 0, Bytes};
  RtlCopyString(&Copied, &Name);
  if (RtlInitUnicodeStringEx(&Bounded, (const U16 *)L"Sample") ||
      Bounded.Length != 12 || Bounded.MaximumLength != 14 ||
      Copied.Length != 6 || Bytes[6] != '!' ||
      !RtlEqualString(&Copied, &Name, 0))
    return (NTSTATUS)0xc000000dU;
  DbgPrint("bytes %d %d", RtlCompareString(&Name, &Empty, 0),
           RtlCompareString(&Empty, &Name, 0));
  return 0;
}
