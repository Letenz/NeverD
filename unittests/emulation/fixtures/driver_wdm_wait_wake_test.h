//===- driver_wdm_wait_wake_test.h - Native wake test protocol -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DRIVER_WDM_WAIT_WAKE_H
#define NEVERD_TESTS_DRIVER_WDM_WAIT_WAKE_H

enum {
  WdmWakeSnapshotIoctl = 0x222000,
  WdmWakeCommandIoctl = 0x222004,
  WdmWakeRearm = 'A',
  WdmWakeCancel = 'C',
  WdmWakeCancelDpc = 'D',
  WdmWakeCancelRearm = 'X',
  WdmWakeRearmOnSuccess = 'R',
  WdmWakeD0OnSuccess = 'P',
  WdmWakeModeWorking = 'W',
  WdmWakeModeSleeping = 'S',
  WdmWakeModeNoCallback = 'N',
  WdmWakeModeNoOutput = 'O',
  WdmWakeModePDO = 'B',
  WdmWakeCancelledStatus = 0xc0000120U,
  WdmWakeFailures = 0,
  WdmWakeSubmissions,
  WdmWakeDispatches,
  WdmWakeIoCompletions,
  WdmWakeCallbacks,
  WdmWakeCancelCalls,
  WdmWakeCancelTrue,
  WdmWakeActive,
  WdmWakeLastStatus,
  WdmWakeLastIoIrql,
  WdmWakeLastCallbackIrql,
  WdmWakeD0Callbacks,
  WdmWakeDeviceDispatches,
  WdmWakeWorkers,
  WdmWakeStarts,
  WdmWakeStops,
  WdmWakeLastLimit,
  WdmWakePublishedOutputs,
  WdmWakeIRPLow,
  WdmWakeIRPHigh,
  WdmWakeHasCancelRoutine,
  WdmWakeSnapshotWords
};

#endif // NEVERD_TESTS_DRIVER_WDM_WAIT_WAKE_H
