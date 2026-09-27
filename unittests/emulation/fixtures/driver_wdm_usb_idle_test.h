//===- driver_wdm_usb_idle_test.h - USB idle fixture protocol ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DRIVER_WDM_USB_IDLE_H
#define NEVERD_TESTS_DRIVER_WDM_USB_IDLE_H

enum {
  UsbIdleCommandIoctl = 0x222000,
  UsbIdleSnapshotIoctl = 0x222004,
  UsbIdleInternalIoctl = 0x220027,
  UsbIdleSubmit = 'S',
  UsbIdleSubmitWithCancelWorker = 'I',
  UsbIdleSubmitWithRemoteWake = 'W',
  UsbIdleSubmitThroughStack = 'F',
  UsbIdleSubmitDuplicate = 'B',
  UsbIdleCancel = 'C',
  UsbIdleCancelRearm = 'X',
  UsbIdleRequestD0 = '0',
  UsbIdleRequestD3 = '3',
  UsbIdleCallerStackMode = 'A',
  UsbIdleCancelledStatus = 0xc0000120U,
  UsbIdleBusyStatus = 0x80000011U,
  UsbIdlePowerInvalidStatus = 0xc00002d3U,
  UsbIdleFailures = 0,
  UsbIdleAllocations,
  UsbIdleDispatchReturns,
  UsbIdleLastDispatchStatus,
  UsbIdleActive,
  UsbIdleHasCancelRoutine,
  UsbIdleCallbackEntries,
  UsbIdleCallbackReturns,
  UsbIdleD2Completions,
  UsbIdleCompletions,
  UsbIdleFrees,
  UsbIdleLastStatus,
  UsbIdleCancelCalls,
  UsbIdleCancelTrue,
  UsbIdleCancelWorkers,
  UsbIdleRetainedAfterInCallbackCancel,
  UsbIdleD0Completions,
  UsbIdleD3Completions,
  UsbIdleCompletedBeforeD0Acknowledgement,
  UsbIdleWakeSubmissions,
  UsbIdleWakeCallbacks,
  UsbIdleLastWakeStatus,
  UsbIdleWakeActive,
  UsbIdleDevicePower,
  UsbIdleSystemPower,
  UsbIdleEntryOrder,
  UsbIdleD2CompletionOrder,
  UsbIdleReturnOrder,
  UsbIdleCompletionOrder,
  UsbIdleD0DispatchOrder,
  UsbIdleD0CompletionOrder,
  UsbIdleLastCompletionIrql,
  UsbIdleLastCompletionDeviceIsSelf,
  UsbIdleInternalDispatches,
  UsbIdleStarts,
  UsbIdleStops,
  UsbIdleSnapshotWords
};

#endif // NEVERD_TESTS_DRIVER_WDM_USB_IDLE_H
