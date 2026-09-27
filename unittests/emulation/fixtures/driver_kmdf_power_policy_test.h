//===- driver_kmdf_power_policy_test.h - Power policy fixture contract ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_DRIVER_KMDF_POWER_POLICY_TEST_H
#define NEVERD_DRIVER_KMDF_POWER_POLICY_TEST_H

enum {
  KmdfPowerIdleWake = 'A',
  KmdfPowerIdleOnly = 'B',
  KmdfPowerSystemWake = 'C',
  KmdfPowerArmFailure = 'D',
  KmdfPowerSystemArmFailure = 'E',
  KmdfPowerManagedQueue = 'F',
  KmdfPowerManagedWait = 'G',
  KmdfPowerIdleAndSystemWake = 'H',
  KmdfPowerRemainIdleOnSystemWake = 'I',
  KmdfPowerColdExplicit = 'J',
  KmdfPowerColdDefault = 'K',
  KmdfPowerColdExcluded = 'L',
  KmdfPowerColdDefaultHot = 'M',
  KmdfPowerColdNoWake = 'N',
  KmdfPowerColdWakeUnavailable = 'O',
  KmdfPowerColdSystemWake = 'P',
  KmdfPowerSystemManaged = 'Q',
  KmdfPowerSystemManagedHint = 'R',
  KmdfPowerSnapshot = 0,
  KmdfPowerHold = 1,
  KmdfPowerRelease = 2,
  KmdfPowerWaitD0 = 3,
  KmdfPowerWaitD0Worker = 4,
  KmdfPowerReadRegister = 5,
  KmdfPowerTimeoutMs = 1,
  KmdfPowerTimeout100ns = 10000,
  KmdfPowerSnapshotWords = 6,
  KmdfPowerColdSnapshotWords = 8,
  KmdfPowerRegisterMarker = 0x13572468
};
#endif
