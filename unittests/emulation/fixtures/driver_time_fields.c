//===- driver_time_fields.c - Original calendar time fixture --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Converts between system times and calendar fields the way driver logging
/// does, without host or WDK dependencies.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned int U32;
typedef long long I64;
typedef int NTSTATUS;
typedef struct {
  short Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
} TIME_FIELDS;

__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) void RtlTimeToTimeFields(I64 *, TIME_FIELDS *);
__declspec(dllimport) U8 RtlTimeFieldsToTime(TIME_FIELDS *, I64 *);

NTSTATUS DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  TIME_FIELDS Fields = {2024, 2, 29, 23, 59, 58, 999, 0};
  I64 Time = 0, Back = 0;
  if (!RtlTimeFieldsToTime(&Fields, &Time))
    return (NTSTATUS)0xc000000dU;
  TIME_FIELDS Out;
  RtlTimeToTimeFields(&Time, &Out);
  if (!RtlTimeFieldsToTime(&Out, &Back) || Back != Time)
    return (NTSTATUS)0xc000000dU;
  // An impossible date is rejected and leaves its output untouched.
  TIME_FIELDS Invalid = {2023, 2, 29, 0, 0, 0, 0, 0};
  I64 Untouched = -1;
  U8 Accepted = RtlTimeFieldsToTime(&Invalid, &Untouched);
  DbgPrint("time %04d-%02d-%02d %02d:%02d:%02d.%03d weekday %d %d %d", Out.Year,
           Out.Month, Out.Day, Out.Hour, Out.Minute, Out.Second,
           Out.Milliseconds, Out.Weekday, Accepted, Untouched == -1);
  return 0;
}
