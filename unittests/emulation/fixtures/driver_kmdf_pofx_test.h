//===- driver_kmdf_pofx_test.h - Shared custom-component fixture contract ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Service modes and snapshot layout shared by the WDK guest and host tests.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_DRIVER_KMDF_POFX_TEST_H
#define NEVERD_DRIVER_KMDF_POFX_TEST_H

enum {
  KmdfPoFxNormal = 'N',
  KmdfPoFxDeferredIdle = 'A',
  KmdfPoFxDefaultConditions = 'O',
  KmdfPoFxSelfManagedSettings = 'S',
  KmdfPoFxFailPostRegister = 'F',
  KmdfPoFxDefaultComponent = 'Z',
  KmdfPoFxSnapshotIoctl = 0x222000,
  KmdfPoFxSnapshotFailures = 0,
  KmdfPoFxSnapshotPosts = 1,
  KmdfPoFxSnapshotPres = 2,
  KmdfPoFxSnapshotActiveConditions = 3,
  KmdfPoFxSnapshotIdleConditions = 4,
  KmdfPoFxSnapshotF0Transitions = 5,
  KmdfPoFxSnapshotF1Transitions = 6,
  KmdfPoFxSnapshotCurrentState = 7,
  KmdfPoFxSnapshotD0Entries = 8,
  KmdfPoFxSnapshotD0Exits = 9,
  KmdfPoFxSnapshotWords = 10,
  KmdfPoFxIdleTimeoutMilliseconds = 1
};
#endif
