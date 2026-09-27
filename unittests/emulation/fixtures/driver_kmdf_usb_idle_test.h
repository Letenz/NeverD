//===- driver_kmdf_usb_idle_test.h - Genuine USB policy protocol ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DRIVER_KMDF_USB_IDLE_H
#define NEVERD_TESTS_DRIVER_KMDF_USB_IDLE_H

enum {
  KmdfUsbInternalIdleIoctl = 0x220027,
  KmdfUsbCancelledStatus = 0xc0000120U,
  KmdfUsbSnapshotIoctl = 0x222000,
  KmdfUsbConfigureIoctl = 0x222004,
  KmdfUsbStopIdleIoctl = 0x222008,
  KmdfUsbResumeIdleIoctl = 0x22200c,
  KmdfUsbDirectReadIoctl = 0x222010,
  KmdfUsbPoFxSnapshotIoctl = 0x222014,
  KmdfUsbBehaviorIoctl = 0x222018,
  KmdfUsbFailArmBehavior = 1,
  KmdfUsbStopInArmBehavior = 2,
  KmdfUsbSystemManagedMode = 'S',
  KmdfUsbSystemManagedHintMode = 'H',
  KmdfUsbReadMarker = 0x55425349,
  KmdfUsbIdleTimeoutMilliseconds = 1,
  KmdfUsbIdleTimeout100ns = 10000,
  KmdfUsbConfigIdentity = 0,
  KmdfUsbConfigEnabled,
  KmdfUsbConfigMaximum,
  KmdfUsbConfigFailArm,
  KmdfUsbConfigStopInArm,
  KmdfUsbConfigurationBytes,
  KmdfUsbFailures = 0,
  KmdfUsbIdentity,
  KmdfUsbD0Entries,
  KmdfUsbD0Exits,
  KmdfUsbArms,
  KmdfUsbDisarms,
  KmdfUsbTriggers,
  KmdfUsbInD0,
  KmdfUsbArmed,
  KmdfUsbReadsRouted,
  KmdfUsbReadsDelivered,
  KmdfUsbStopCalls,
  KmdfUsbStopStatus,
  KmdfUsbResumeCalls,
  KmdfUsbPreviousState,
  KmdfUsbTargetState,
  KmdfUsbEntrySequence,
  KmdfUsbReadRouteSequence,
  KmdfUsbReadDeliverySequence,
  KmdfUsbSnapshotWords,
  KmdfUsbPoFxPosts = KmdfUsbSnapshotWords,
  KmdfUsbPoFxPres,
  KmdfUsbPoFxIdleConditions,
  KmdfUsbPoFxActiveConditions,
  KmdfUsbPoFxActive,
  KmdfUsbPoFxActiveSequence,
  KmdfUsbPoFxF0Transitions,
  KmdfUsbPoFxF1Transitions,
  KmdfUsbPoFxCurrentState,
  KmdfUsbPoFxF0Sequence,
  KmdfUsbPoFxSnapshotWords
};
#endif
