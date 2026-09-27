//===- driver_kmdf_child_wake_test.h - Parent and child wake protocol -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Share the genuine driver's configuration and observation protocol with the
/// host tests without depending on WDK types or host object layout.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DRIVER_KMDF_CHILD_WAKE_H
#define NEVERD_TESTS_DRIVER_KMDF_CHILD_WAKE_H

enum {
  KmdfChildWakeSnapshotIoctl = 0x222000,
  KmdfChildWakeConfigureIoctl = 0x222004,
  KmdfChildWakeIdentity = 0,
  KmdfChildWakeOwnEnabled,
  KmdfChildWakeArmForChildren,
  KmdfChildWakePropagate,
  KmdfChildWakeFailArm,
  KmdfChildWakeConfigurationBytes,
  KmdfChildWakeFailures = 0,
  KmdfChildWakeDeviceIdentity,
  KmdfChildWakeD0Entries,
  KmdfChildWakeD0Exits,
  KmdfChildWakeArms,
  KmdfChildWakeTriggers,
  KmdfChildWakeDisarms,
  KmdfChildWakeInD0,
  KmdfChildWakeArmed,
  KmdfChildWakeOwnReason,
  KmdfChildWakeChildrenReason,
  KmdfChildWakeSnapshotWords
};
#endif
