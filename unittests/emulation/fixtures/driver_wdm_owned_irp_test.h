//===- driver_wdm_owned_irp_test.h - Caller-owned IRP test protocol -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Share command and snapshot identities between genuine WDK driver code and
/// host tests without sharing platform-dependent structures.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DRIVER_WDM_OWNED_IRP_H
#define NEVERD_TESTS_DRIVER_WDM_OWNED_IRP_H

enum {
  OwnedIrpSubmitIoctl = 0x222000,
  OwnedIrpSnapshotIoctl = 0x222004,
  OwnedIrpReleaseIoctl = 0x222008,
  OwnedIrpInternalIoctl = 0x222003,
  OwnedIrpInline = 'I',
  OwnedIrpWorker = 'W',
  OwnedIrpHold = 'H',
  OwnedIrpCancel = 'C',
  OwnedIrpNested = 'N',
  OwnedIrpUnsent = 'U',
  OwnedIrpKernelBuffer = 'B',
  OwnedIrpBufferInitial = 0x31465249,
  OwnedIrpBufferWritten = 0x32465249,
  OwnedIrpFailures = 0,
  OwnedIrpAllocations,
  OwnedIrpLowerDispatches,
  OwnedIrpCompletions,
  OwnedIrpFrees,
  OwnedIrpHeld,
  OwnedIrpCancelCalls,
  OwnedIrpWorkers,
  OwnedIrpNestedCompletions,
  OwnedIrpLastStatus,
  OwnedIrpLastPending,
  OwnedIrpLastDeviceIsUpper,
  OwnedIrpDispatchReturns,
  OwnedIrpLastDispatchStatus,
  OwnedIrpSnapshotWords
};

#endif // NEVERD_TESTS_DRIVER_WDM_OWNED_IRP_H
