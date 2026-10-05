//===- driver_seh_test.h - Shared genuine C SEH fixture contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_DRIVER_SEH_TEST_H
#define NEVERD_DRIVER_SEH_TEST_H

enum {
  SehXmmUnwind = 'x',
  SehChainedUnwind = 'c',
  SehPrologueUnwind = 'p',
  SehGSCookie = 'g',
  SehGSAlignedCookie = 'a',
  SehGSCorruptCookie = 'b',
  SehGSAlignedCorruptCookie = 'd',
  SehGSStandaloneCookie = 's',
  SehGSStandaloneAlignedCookie = 't',
  SehGSStandaloneCorruptCookie = 'u',
  SehGSStandaloneAlignedCorruptCookie = 'v',
  SehDynamicFilter = 'F',
  SehSearchFilters = 'Q',
  SehExceptionalFinally = 'T',
  SehNormalFinally = 'L',
  SehContinueApi = 'E',
  SehNestedFilter = 'B',
  SehNestedFinally = 'J',
  SehLocalFilter = 'I',
  SehLocalFinally = 'O',
  SehNestedSearch = 'P',
  SehRepeatedFilter = 'X',
  SehRecoverUserRead = 'V',
  SehRejectContextMutation = 'M',
  SehWorkerUserProbe = 'W',
  SehWorkerUserLock = 'K',
  SehAttachedWorker = 'Z',
  SehAttachedForbidden = 'Y',
  SehFilterUserMarker = 0x516e4a23,
  SehRecoverControlCode = 0x222403,
  SehRecoveredValue = 0x12345678
};
enum {
#define NEVERD_SEH_CPU_VALUE(Name, Value) Name = Value,
#include "driver_seh_cpu.def"
#undef NEVERD_SEH_CPU_VALUE
};
#define NEVERD_SEH_CPU_TEXT(Name, Text) static const char Name[] = Text;
#include "driver_seh_cpu.def"
#undef NEVERD_SEH_CPU_TEXT
enum {
#define NEVERD_SEH_SIMD_VALUE(Name, Value) Name = Value,
#define NEVERD_SEH_SIMD_MODE(Name, Value) SehSIMD##Name = Value,
#include "driver_seh_simd.def"
#undef NEVERD_SEH_SIMD_MODE
#undef NEVERD_SEH_SIMD_VALUE
};
#define NEVERD_SEH_SIMD_TEXT(Name, Text) static const char Name[] = Text;
#include "driver_seh_simd.def"
#undef NEVERD_SEH_SIMD_TEXT
#endif
