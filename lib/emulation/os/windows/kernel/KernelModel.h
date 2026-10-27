//===- KernelModel.h - Bounded Windows environment ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded Windows environment model.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELMODEL_H
#define NEVERD_EMULATION_KERNELMODEL_H
#include "DeviceLifecycle.h"
#include "KernelDMA.h"
#include "KernelDispatcher.h"
#include "KernelFileSystem.h"
#include "KernelFramework.h"
#include "KernelGuestCall.h"
#include "KernelInterrupts.h"
#include "KernelMMIO.h"
#include "KernelModelMemory.h"
#include "KernelModuleImages.h"
#include "KernelPoFx.h"
#include "KernelRegistry.h"
#include "KernelRemoveLocks.h"
#include "KernelScheduler.h"
#include "KernelUsbIdle.h"
#include "KernelWaits.h"

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/GuestMemory.h"
#include "neverd/emulation/WindowsDriverState.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <deque>
#include <map>
#include <optional>
#include <set>
#include <variant>
namespace neverd::emulation {
class RAMInitializationWatch;
struct DriverImage;
class KernelExportRegistry;
struct KernelContextCapture;
enum class KernelAPIKind;
class KernelModel {
  friend class KernelRegistry;

public:
  KernelModel(GuestMemory &Backing, DriverResult &Result,
              KernelExportRegistry *Exports = nullptr)
      : ObservedMemory(Backing, [this] { recordUnpackEnvironmentRead(); }),
        Memory(ObservedMemory), Result(Result), Exports(Exports),
        Registry(*this, Memory),
        Dispatcher(Memory, Scheduler,
                   [this](uint64_t Address, uint32_t Size, bool IsWrite) {
                     return validateDispatcherStorage(Address, Size, IsWrite);
                   }),
        Resources([this](uint64_t PDO) { return canReleaseResources(PDO); },
                  [this](uint64_t PDO) {
                    // DMA adapter acquisition in AddDevice precedes START.
                    return llvm::joinErrors(MMIO.canRemove(PDO),
                                            Interrupts.canRelease(PDO));
                  },
                  [this](uint64_t PDO) { return DMA.canPowerDownPDO(PDO); }),
        MMIO(Memory, Resources), Interrupts(Resources, Result),
        Physical(Memory), DMA(Physical, Resources, Result) {}
  KernelModel(const KernelModel &) = delete;
  KernelModel &operator=(const KernelModel &) = delete;
  KernelModel(KernelModel &&) = delete;
  KernelModel &operator=(KernelModel &&) = delete;
  llvm::Error initialize(const DriverImage &Image,
                         const DriverOptions &Options);
  /// End a normally returned DriverEntry, regardless of its NTSTATUS. The
  /// borrowed RegistryPath record and buffer expire before any later callback.
  llvm::Error finishEntry();
  /// A stopped DriverEntry can be rebuilt only when borrowed loader objects
  /// retain their initial state and every retained kernel effect is known.
  llvm::Error captureUnpackBaseline();
  void recordUnpackEnvironmentRead();
  llvm::Expected<std::vector<std::string>> unpackDependencies() const;
  llvm::Expected<WindowsDriverState>
  captureUnpackRuntime(bool IncludeBacking) const;
  std::map<uint64_t, uint64_t> unpackAllocations() const;
  uint64_t driverObject() const { return DriverObject; }
  uint64_t registryPath() const { return RegistryPath; }
  /// Fixed kernel argument counts; unknown names have no execution contract.
  static std::optional<unsigned> argumentCount(const std::string &Name);
  static std::optional<unsigned>
  argumentCount(const KernelExportRegistry::Export &Export);
  llvm::Expected<uint64_t>
  call(const KernelExportRegistry::Export &Export,
       llvm::ArrayRef<uint64_t> Arguments,
       llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument,
       const KernelContextCapture *Context = nullptr);
  std::optional<KernelGuestCall> takeGuestCall();
  std::optional<KernelGuestCall> takeIndependentGuestCall();
  std::optional<KernelGuestCall> takePoFxThreadCall(uint64_t Thread);
  llvm::Error beginGuestCall(GuestCallToken Token);
  llvm::Expected<bool> enterFrameworkCallback(uint64_t ScheduledID,
                                              GuestCallToken Token,
                                              uint64_t IRP);
  void setFrameworkCallbackContext(bool PowerManaged) {
    FrameworkPowerManagedCallback = PowerManaged;
  }
  llvm::Expected<std::optional<uint64_t>> finishGuestCall(GuestCallToken Token,
                                                          uint64_t Result);
  llvm::Expected<uint64_t>
  call(const std::string &Name, llvm::ArrayRef<uint64_t> Arguments,
       llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument =
           nullptr,
       const KernelContextCapture *Context = nullptr);
  /// Deliver completed file notifications at an eligible caller boundary.
  /// File retirement and API results remain owned by the file continuation.
  llvm::Error
  deliverFileRequestCompletions(const KernelContextCapture *Context = nullptr);
  llvm::Error snapshot();
  struct Invocation {
    uint64_t PC = 0;
    uint64_t Argument0 = 0;
    uint64_t Argument1 = 0;
    uint64_t Argument2 = 0;
    uint64_t Argument3 = 0;
    std::vector<uint64_t> StackArguments;
    std::optional<uint32_t> FrameworkDispatchStatus;
    bool FrameworkCallerContext = false;
    uint64_t IRP = 0;
    uint64_t SynchronizationObject = 0;
  };
  // PnP device enrollment: configuration identities never expose addresses.
  llvm::Error preparePnpDevices();
  llvm::Expected<Invocation> beginAddDevice(llvm::StringRef ID);
  llvm::Error finishAddDevice(llvm::StringRef ID, uint32_t Status);
  llvm::Error finalizeAddDevice(llvm::StringRef ID);
  llvm::Error beginFrameworkRemoval(uint64_t IRP);
  /// A pending dispatch retains its packet until a guest callback completes it.
  llvm::Expected<Invocation>
  beginRequest(const DriverRequest &Request,
               std::optional<size_t> SourceIndex = std::nullopt);
  llvm::Expected<Invocation> continueFrameworkCallerContext(uint64_t IRP);
  llvm::Error recordDispatchReturn(uint64_t IRP, uint32_t DispatchStatus);
  /// Acquire the callback's effective device or queue synchronization lock.
  /// A false result suspends entry; the real guest frame has not run yet.
  llvm::Expected<bool> beginFrameworkCallback(uint64_t Object);
  llvm::Error finishFrameworkCallback(uint64_t Object);
  llvm::Error flushFrameworkCallbackDestructions();
  /// Finalization is idempotent only for an already finalized owned IRP.
  llvm::Error finalizeRequest(uint64_t IRP);
  /// IRP=0 inspects all requests. The scheduler can exclude framework packets
  /// parked by device power so a later START can produce their completion.
  enum class PendingRequestScope { All, ExcludePowerParked };
  bool
  requestPending(uint64_t IRP = 0,
                 PendingRequestScope Scope = PendingRequestScope::All) const;
  llvm::Expected<std::optional<KernelScheduler::Invocation>>
  nextScheduled(bool AdvanceTime,
                std::optional<uint64_t> Deadline = std::nullopt);
  llvm::Error finishScheduled(uint64_t ID);
  std::optional<uint32_t> takeThreadTermination();
  /// Finish a scheduled framework callback, including any destruction callbacks
  /// required before releasing its scheduler ownership.
  llvm::Expected<std::optional<KernelGuestCall>>
  continueScheduled(uint64_t ID, uint64_t ReturnValue);
  llvm::Error suspendScheduled(uint64_t ID);
  llvm::Error resumeScheduled(uint64_t ID);
  /// Selector state only. Resource ownership remains in the model's canonical
  /// per-execution and per-thread tables while this continuation is parked.
  struct ExecutionContext {
    uint64_t Execution, Thread;
    GuestCallToken Call;
    uint32_t Process;
    uint8_t IRQL;
    bool UserMemory, PowerManaged;
  };
  ExecutionContext captureExecutionContext() const;
  llvm::Error restoreExecutionContext(const ExecutionContext &Context);
  llvm::Error preemptScheduled(uint64_t ID);
  llvm::Expected<uint64_t> issueReadyOrder() {
    return Scheduler.issueReadyOrder();
  }
  std::optional<uint64_t> nextPassiveReadyOrder() const {
    return Scheduler.nextPassiveReadyOrder();
  }
  std::optional<KernelScheduler::ReadyThread> nextPassiveThread() const {
    return Scheduler.nextPassiveThread();
  }
  KernelScheduler::ReadyThread readyThread(uint64_t Key, uint64_t Order) const {
    return Scheduler.readyThread(Key, Order);
  }
  int32_t threadPriority(uint64_t Key) const {
    return Scheduler.threadPriority(Key);
  }
  uint64_t now100ns() const { return Scheduler.now100ns(); }
  /// Process exactly one chronological boundary, including during execution.
  llvm::Error advanceExecutionTo100ns(uint64_t Time);
  llvm::Error restoreWaitIRQL(uint8_t IRQL);
  struct Wait {
    enum class Kind {
      Dispatcher,
      Thread,
      Multiple,
      Delay,
      RemoveLock,
      FrameworkQueueStop,
      FrameworkQueueEmpty,
      FrameworkFileSend,
      FrameworkIdle,
      PoFxActive,
      PoFxIdle,
      InterruptSynchronization,
      FrameworkInterruptLock,
      FrameworkWaitLock,
      FrameworkCallback,
      FrameworkCallbackLock,
      FileClose,
      FileHandleClose,
      FileRequest,
      FileOperation
    };
    Kind Type = Kind::Dispatcher;
    uint64_t Object = 0;
    uint64_t Execution = 0;
    uint64_t Thread = 0;
    uint8_t IRQL = 0;
    std::optional<uint64_t> Deadline;
    std::vector<uint64_t> Objects;
    bool All = false;
    uint64_t WaitBlockArray = 0;
    uint32_t WaitBlockSize = 0;
    uint64_t Registration = 0;
    bool operator==(const Wait &) const = default;
  };
  std::optional<Wait> takeWait();
  llvm::Expected<std::optional<uint32_t>> pollWait(const Wait &Pending);
  llvm::Expected<std::optional<uint64_t>> resumeFileHandleClose(uint64_t ID);
  std::optional<uint64_t> takeFileRequestContinuation();
  llvm::Expected<std::optional<uint64_t>> resumeFileRequest(uint64_t ID);
  struct FileAPIContinuation {
    bool Request = false;
    uint64_t ID = 0;
  };
  std::optional<FileAPIContinuation> takeFileAPIContinuation();
  llvm::Expected<std::optional<uint64_t>>
  resumeFileAPI(FileAPIContinuation Continuation);
  std::optional<uint64_t> nextEventTime() const;
  bool hasQueuedDPC() const { return Scheduler.hasQueuedDPC(); }
  bool hasQueuedPriorityCallback() const {
    const auto Interrupt = nextInterruptEventTime();
    const auto Transfer = DMA.nextEventTime();
    return (Interrupt && *Interrupt <= Scheduler.now100ns()) ||
           (Transfer && *Transfer <= Scheduler.now100ns()) ||
           Scheduler.hasQueuedInterrupt() || hasQueuedDPC() ||
           Scheduler.hasQueuedDMACallback() ||
           Scheduler.hasQueuedCancellation() || Scheduler.hasQueuedCompletion();
  }
  llvm::Error activateStack(uint64_t Base, uint64_t Size);
  llvm::Error retireStack(uint64_t Base, uint64_t Size);
  void enterForeground() { CurrentIRQL = 0; }
  GuestCallToken scheduledGuestCall(uint64_t ID) const {
    const auto Found = ScheduledModelContinuations.find(ID);
    return Found == ScheduledModelContinuations.end() ? GuestCallToken{}
                                                      : Found->second;
  }
  void enterExecution(uint64_t Identity, uint64_t ThreadKey = 0,
                      GuestCallToken Call = {}) {
    CurrentExecution = Identity;
    CurrentGuestCall = Call;
    CurrentThreadKey = ThreadKey ? ThreadKey : Identity;
    ExecutionThreadKeys[Identity] = CurrentThreadKey;
    if (EnteringPassiveInterrupt) {
      PassiveInterruptThreads.emplace(*EnteringPassiveInterrupt,
                                      CurrentThreadKey);
      EnteringPassiveInterrupt.reset();
    }
    if (Scheduler.active() &&
        Scheduler.active()->Kind ==
            KernelScheduler::CallbackKind::SystemThread &&
        CurrentThreadKey == Scheduler.active()->ID)
      ApcStates.try_emplace(CurrentThreadKey, ApcState{1, 0});
    if (CancelLock.Held && CancelLock.Callback && !CancelLock.Owner) {
      CancelLock.Owner = Identity;
      CancelLock.CallbackExecution = Identity;
    }
  }
  /// A synchronous nested callback retains its parent's process and memory
  /// authority while keeping its own execution and resource ownership.
  llvm::Error
  inheritExecutionContext(uint64_t Child, uint64_t Parent,
                          std::optional<uint32_t> ProcessID = std::nullopt);
  llvm::Error activateInheritedProcess();
  llvm::Error setUserRequestContext(
      bool Active,
      uint32_t ProcessID = DriverRequest::DefaultRequestorProcessID);
  llvm::Error revokeRequestUserBuffers(uint64_t IRP);
  llvm::Error exitRequestorProcess(uint64_t IRP);
  bool canCatchUserAccess(uint64_t Address, uint64_t Size) const;
  llvm::Error validateExecutionReturn(uint64_t Identity, uint8_t EntryIRQL,
                                      bool Nested = false) const;
  bool hasPendingInterruptEvents() const {
    return Interrupts.hasPendingEvents();
  }
  bool hasPendingHardwareWork() const {
    return Interrupts.hasPendingEvents() || DMA.hasPendingEvents() ||
           DMA.hasPendingCallbacks() || PoFx.hasPendingCallbacks() ||
           PoFx.nextDeadline().has_value() || hasPendingPowerPolicyEvents() ||
           (Framework && Framework->hasPendingPowerPolicy());
  }
  uint8_t currentIRQL() const { return CurrentIRQL; }
  /// Return a borrowed, opaque identity for the current kernel thread.
  llvm::Expected<uint64_t> currentThreadObject();
  llvm::Expected<Invocation> beginUnload();
  llvm::Error finishUnload();
  /// Reject CPU accesses whose environment semantics this profile does not own.
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;

private:
  llvm::Expected<uint64_t> dispatchKernelCall(
      const std::string &Name, llvm::ArrayRef<uint64_t> Arguments,
      llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument,
      const KernelContextCapture *Context);
  static std::optional<unsigned> halArgumentCount(llvm::StringRef Name);
  llvm::Expected<uint64_t> callHAL(llvm::StringRef Name,
                                   llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> queryPerformanceCounter(uint64_t Frequency);
  llvm::Expected<uint64_t> setCancelRoutine(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> mapMDL(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> unmapMDL(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> callPowerDriver(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> completeDriverRequest(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t>
  currentDriverRequestStack(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> initializeUnicodeString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> ansiToUnicodeString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> unicodeToAnsiString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> upcaseUnicodeString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> freeCountedString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> stringFromGUID(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> guidFromString(llvm::ArrayRef<uint64_t> A);
  enum class PoolSource { GuestAllocation, KernelResult };
  llvm::Expected<uint64_t>
  allocatePool(llvm::ArrayRef<uint64_t> A, bool Modern,
               PoolSource Source = PoolSource::GuestAllocation);
  llvm::Expected<uint64_t> freePool(llvm::ArrayRef<uint64_t> A, bool Tagged);
  llvm::Expected<uint64_t>
  deleteSymbolicLinkFromGuest(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t>
  createSymbolicLinkFromGuest(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> debugMessage(
      llvm::ArrayRef<uint64_t> A, unsigned FormatIndex,
      llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument);
  llvm::Expected<uint64_t> prefixedDebugMessage(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> checkedMemoryCopy(llvm::ArrayRef<uint64_t> A,
                                             bool ClearOnError);
  llvm::Expected<uint64_t> memoryCall(KernelAPIKind Kind,
                                      llvm::ArrayRef<uint64_t> A);

  KernelModelMemory ObservedMemory;
  GuestMemory &Memory;
  llvm::Expected<std::shared_ptr<RAMInitializationWatch>>
  watchInitialization(uint64_t Address, uint64_t Size);
  static bool initialized(llvm::ArrayRef<bool> Flags,
                          const std::shared_ptr<RAMInitializationWatch> &Watch,
                          uint64_t Offset, uint64_t Size = 1);
  static bool
  anyInitialized(llvm::ArrayRef<bool> Flags,
                 const std::shared_ptr<RAMInitializationWatch> &Watch,
                 uint64_t Offset, uint64_t Size);
  DriverResult &Result;
  KernelExportRegistry *Exports;
  std::unique_ptr<KernelFramework> Framework;
  KernelPoFx PoFx;
  std::map<uint64_t, uint64_t> PoFxDeviceObjects;
  struct FrameworkPoFxPowerWait {
    uint64_t CallbackToken;
    KernelPoFx::CallbackKind Kind;
  };
  std::map<uint64_t, FrameworkPoFxPowerWait> FrameworkPoFxPowerWaits;
  struct BlockingPoFxOperation {
    uint64_t Handle = 0, Thread = 0;
    uint32_t Component = 0;
    bool Active = false;
    std::deque<KernelGuestCall> Calls;
    std::optional<uint64_t> TerminalCallback;
    bool Completed = false;
    uint64_t CompletionGeneration = 0;
  };
  std::map<uint64_t, BlockingPoFxOperation> BlockingPoFx;
  llvm::Error waitForPoFxOperation(uint64_t Thread);
  enum class PoFxHandleAccess { DriverOwned, AnyOwner };
  enum class PoFxCondition { Active, Idle };
  llvm::Error preparePoFxOperation(uint64_t Handle, PoFxHandleAccess Access);
  llvm::Expected<uint64_t> finishPoFxOperation(llvm::Error Error);
  llvm::Expected<uint64_t> registerPoFxDevice(uint64_t Object, uint64_t Record,
                                              uint64_t Output);
  llvm::Expected<uint64_t> unregisterPoFxDevice(uint64_t Handle);
  llvm::Expected<uint64_t> startPoFxPowerManagement(uint64_t Handle);
  llvm::Expected<uint64_t> changePoFxComponent(uint64_t Handle, uint32_t Index,
                                               uint32_t Flags,
                                               PoFxCondition Condition);
  llvm::Expected<uint64_t> completePoFxIdleCondition(uint64_t Handle,
                                                     uint32_t Index);
  llvm::Expected<uint64_t> completePoFxIdleState(uint64_t Handle,
                                                 uint32_t Index);
  llvm::Expected<uint64_t> completePoFxPowerNotRequired(uint64_t Handle);
  llvm::Expected<uint64_t> reportPoFxDevicePoweredOn(uint64_t Handle);
  llvm::Expected<uint64_t>
  setPoFxComponentLatency(uint64_t Handle, uint32_t Index, uint64_t Latency);
  llvm::Expected<uint64_t> setPoFxComponentResidency(uint64_t Handle,
                                                     uint32_t Index,
                                                     uint64_t Residency);
  llvm::Expected<uint64_t> setPoFxComponentWake(uint64_t Handle, uint32_t Index,
                                                bool Wake);
  llvm::Expected<uint64_t> setPoFxDeviceIdleTimeout(uint64_t Handle,
                                                    uint64_t Timeout);
  llvm::Expected<KernelPoFx::Registration>
  readPoFxRegistration(uint64_t PDO, uint64_t Address);
  llvm::Error queuePoFxCallbacks();
  llvm::Error processPoFxCallbacks();
  llvm::Expected<std::optional<uint64_t>> finishPoFxCall(uint64_t Token);
  llvm::Error processInternalPoFxCallback(const KernelPoFx::Callback &Call);
  llvm::Expected<KernelPoFx::Component> readPoFxComponent(uint64_t Address);
  llvm::Error validatePoFxRegistrationDevice(uint64_t PDO) const;
  void configureFrameworkPoFxHost();
  llvm::Error setFrameworkPoFxIdle(uint64_t Device, bool Idle,
                                   uint64_t Timeout);
  llvm::Error retireFrameworkPoFx(uint64_t Device);
  llvm::Error completeFrameworkPowerNotRequired(
      uint64_t Device, KernelFramework::PowerPolicyHost::RequestMode Mode);

  std::optional<KernelGuestCall> takeWdmGuestCall();
  llvm::Expected<std::optional<uint64_t>> finishWdmGuestCall(uint64_t Token,
                                                             uint64_t Result);
  void configureFrameworkDeviceHost();
  void configureFrameworkPowerPolicyHost();
  llvm::Error requestFrameworkDevicePower(
      uint64_t Device, DevicePowerState Target,
      KernelFramework::PowerPolicyHost::RequestMode Mode);
  llvm::Error armFrameworkWake(uint64_t Device, bool SystemSleep);
  llvm::Error finishFrameworkWake(uint64_t Device, bool Triggered);
  llvm::Expected<uint64_t> preflightFrameworkWake(uint64_t PDO,
                                                  uint32_t Status) const;
  llvm::Expected<uint64_t> retainProviderWake(uint64_t PDO, uint64_t IRP);
  llvm::Expected<uint64_t> preflightProviderWake(uint64_t PDO, uint64_t IRP,
                                                 uint32_t Status) const;
  llvm::Error completeProviderWake(uint64_t PDO, uint64_t IRP, uint32_t Status,
                                   std::optional<uint64_t> SourcePDO);
  static std::optional<unsigned> providerArgumentCount(llvm::StringRef Name);
  llvm::Expected<uint64_t>
  callProviderExport(const KernelExportRegistry::Export &Export,
                     llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error completeFrameworkWakes(uint64_t SourceDevice,
                                     llvm::ArrayRef<uint64_t> Devices);
  llvm::Error cancelFrameworkWakes(llvm::ArrayRef<uint64_t> Devices);
  llvm::Error
  canArmPowerPolicyEvents(llvm::ArrayRef<DriverPowerPolicyEvent> Events) const;
  llvm::Error
  armPowerPolicyEvents(llvm::ArrayRef<DriverPowerPolicyEvent> Events,
                       size_t SourceIndex);
  llvm::Error processPowerPolicyEvents();
  std::optional<uint64_t> nextPowerPolicyEventTime() const;
  bool hasPendingPowerPolicyEvents() const;
  struct FrameworkPowerEvent {
    uint64_t Epoch;
  };
  struct WdmWakeEvent {
    uint64_t StartEpoch;
    uint64_t IRP;
  };
  struct PoFxPowerEvent {
    uint64_t Epoch;
    uint64_t Handle;
  };
  struct UsbIdlePermissionEvent {
    uint64_t Epoch;
    std::vector<UsbIdleKey> Members;
  };
  using PowerEventOwner = std::variant<FrameworkPowerEvent, WdmWakeEvent,
                                       PoFxPowerEvent, UsbIdlePermissionEvent>;
  struct PowerPolicyEvent {
    uint64_t PDO;
    size_t ResultIndex;
    PowerEventOwner Owner;
  };
  llvm::Expected<PowerEventOwner>
  capturePowerEventOwner(const DriverPowerPolicyEvent &Event) const;
  std::vector<PowerPolicyEvent> PowerPolicyEvents;
  struct ProviderWake {
    uint64_t IRP = 0;
    uint64_t StartEpoch = 0;
    std::optional<uint64_t> FrameworkEpoch;
    uint64_t CancelRoutine = 0;
  };
  std::map<uint64_t, ProviderWake> ProviderWakeIRPs;
  KernelUsbIdle UsbIdle;
  struct FrameworkUsbIdleRequest {
    uint64_t Device = 0;
    uint64_t PolicyEpoch = 0;
    uint64_t Info = 0;
    UsbIdleKey Key;
  };
  std::map<uint64_t, FrameworkUsbIdleRequest> FrameworkUsbIdleRequests;
  llvm::Expected<KernelFramework::PowerPolicyHost::UsbIdleSettings>
  resolveFrameworkUsbIdle(uint64_t Device, uint32_t RequestedState) const;
  llvm::Expected<UsbIdleKey> submitFrameworkUsbIdle(uint64_t Device,
                                                    uint64_t PolicyEpoch);
  llvm::Expected<bool> hasFrameworkUsbIdle(UsbIdleKey Key) const;
  llvm::Error
  cancelFrameworkUsbIdle(UsbIdleKey Key,
                         KernelFramework::PowerPolicyHost::RequestMode Mode);
  llvm::Error requestFrameworkUsbIdlePower(
      uint64_t Device, UsbIdleKey Key, uint64_t Token,
      KernelFramework::PowerPolicyHost::RequestMode Mode);
  llvm::Error abortFrameworkUsbIdlePower(UsbIdleKey Key, uint64_t Token,
                                         uint32_t Status);
  llvm::Error finishFrameworkUsbIdleCallback(UsbIdleKey Key, uint64_t Token);
  llvm::Error tryFinalizeFrameworkUsbIdle(uint64_t IRP);
  llvm::Expected<std::optional<KernelScheduler::Invocation>>
  dispatchScheduledFrameworkUsbIdle(const KernelScheduler::Invocation &Call);

  const DriverUsbIdleConfig *usbIdleConfig(uint64_t PDO) const;
  llvm::Expected<UsbIdleSubmission>
  planUsbIdleSubmission(uint64_t PDO, uint64_t IRP, uint64_t Info,
                        uint64_t InputSize, uint64_t OutputSize,
                        uint64_t Output) const;
  llvm::Expected<uint64_t> receiveUsbIdle(uint64_t PDO, uint64_t IRP);
  llvm::Expected<std::vector<UsbIdleKey>>
  captureUsbIdlePermission(uint64_t PDO) const;
  llvm::Error validateUsbIdlePermission(llvm::ArrayRef<UsbIdleKey> Keys) const;
  llvm::Expected<std::vector<KernelScheduler::UsbIdleCallback>>
  previewUsbIdleCallbacks(llvm::ArrayRef<UsbIdleKey> Keys) const;
  llvm::Error queueUsbIdlePermission(llvm::ArrayRef<UsbIdleKey> Keys);
  llvm::Error beginUsbIdleCallback(uint64_t Token);
  llvm::Expected<std::optional<uint64_t>> finishUsbIdleCallback(uint64_t Token);
  llvm::Expected<UsbIdleCompletionPlan>
  preflightUsbIdleCompletion(UsbIdleKey Key, UsbIdleCompletionCause Cause,
                             std::optional<bool> CancelOverride = {}) const;
  llvm::Error completeUsbIdle(UsbIdleKey Key, UsbIdleCompletionCause Cause);
  llvm::Expected<uint64_t>
  cancelUsbIdle(const KernelExportRegistry::Export &Export,
                llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error observeUsbIdlePowerCompletion(uint64_t IRP, uint32_t Status);
  llvm::Error validateUsbRemoteWake(uint64_t PDO) const;
  void configureFrameworkInterruptHost();
  void configureFrameworkLockHost();
  llvm::Expected<std::optional<uint32_t>>
  pollFrameworkWaitLock(const Wait &Pending);
  struct FrameworkCallbackLock {
    uint64_t Storage = 0;
  };
  bool FrameworkPowerManagedCallback = false;
  std::map<uint64_t, FrameworkCallbackLock> FrameworkCallbackLocks;
  struct FrameworkCallbackEntry {
    uint64_t Object = 0, Thread = 0;
    uint8_t PreviousIRQL = 0;
    bool Acquired = false;
    bool Passive = true;
    bool Automatic = true;
    uint16_t CriticalDepth = 0;
  };
  std::map<std::pair<uint64_t, uint64_t>, FrameworkCallbackEntry>
      FrameworkCallbackEntries;
  llvm::Expected<uint64_t> frameworkCallbackLock(uint64_t Object);
  llvm::Expected<bool> acquireFrameworkCallback(uint64_t Object,
                                                bool Automatic);
  llvm::Error releaseFrameworkCallback(uint64_t Object, bool Automatic);
  llvm::Expected<std::optional<uint32_t>>
  pollFrameworkCallback(const Wait &Pending);
  llvm::Error completeFrameworkTransitionIfReady();
  llvm::Expected<uint64_t> forwardFrameworkTransitionRequest(uint64_t IRP);
  llvm::Error detachFrameworkPnpDevice(uint64_t Device, uint64_t PDO);
  /// Framework-owned WDM devices and their canonical symbolic-link keys.
  std::map<uint64_t, std::vector<std::string>> FrameworkDevices;
  KernelRegistry Registry;
  KernelFileSystem FileSystem;
  KernelScheduler Scheduler;
  KernelDispatcher Dispatcher;
  KernelRemoveLocks RemoveLocks;
  KernelResources Resources;
  KernelMMIO MMIO;
  KernelInterrupts Interrupts;
  KernelPhysicalMemory Physical;
  KernelDMA DMA;
  std::optional<KernelGuestCall> PendingDMACall;
  std::set<uint64_t> InlineDMACalls;
  bool hasPendingIndependentGuestCall() const {
    return PendingWdmCall || PendingInterruptCall || PendingDMACall ||
           (Framework && Framework->hasPendingGuestCall());
  }
  bool hasPendingModelGuestCall() const {
    auto Operation = BlockingPoFx.find(CurrentThreadKey);
    const bool HasPoFxCall =
        Operation != BlockingPoFx.end() && !Operation->second.Calls.empty();
    return HasPoFxCall || hasPendingIndependentGuestCall();
  }
  static std::optional<unsigned> dmaArgumentCount(llvm::StringRef Name);
  llvm::Expected<uint64_t>
  callDMAExport(const KernelExportRegistry::Export &Export,
                llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> getDMAAdapter(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  allocateCommonBuffer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error freeCommonBuffer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  getScatterGatherList(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error putScatterGatherList(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  allocateAdapterChannel(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> mapTransfer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error flushAdapterBuffers(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error freeMapRegisters(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error flushIoBuffers(uint64_t MDL);
  struct DmaMdlView {
    uint64_t Owner, Offset, DescriptorSize;
  };
  llvm::Expected<DmaMdlView> dmaMdlView(uint64_t MDL, uint64_t CurrentVA,
                                        uint32_t Length, bool ToDevice) const;
  llvm::Expected<std::vector<uint64_t>>
  dmaPromotionIDs(llvm::ArrayRef<KernelDMA::Promotion> Ready) const;
  llvm::Error releaseDMAMapping(const KernelDMA::ReleasePlan &Plan);
  llvm::Error beginDMACall(uint64_t Object);
  llvm::Expected<std::optional<uint64_t>> finishDMACall(uint64_t Object,
                                                        uint64_t Result);

  uint64_t CurrentExecution = 0;
  GuestCallToken CurrentGuestCall;
  uint64_t CurrentThreadKey = 0;
  std::map<uint64_t, uint64_t> ExecutionThreadKeys;
  // DriverSession activates the actual extent before entering an execution.
  // Nested stacks have independent bounds even when they share a thread key.
  std::map<uint64_t, uint64_t> ExecutionStacks;
  llvm::Expected<std::pair<uint64_t, uint64_t>> currentStackBounds() const;
  llvm::Expected<uint64_t> queryInitialStack() const;
  llvm::Expected<uint64_t> queryStackLimits(uint64_t Low, uint64_t High);
  struct ExecutionProcessContext {
    uint32_t ProcessID, CreatingProcessID;
    uint8_t PreviousMode;
    /// Public KeStackAttachProcess currently admits only a bounded operation
    /// set. Internal object-manager callbacks have their own continuation.
    bool RestrictedAttachment;
  };
  std::optional<ExecutionProcessContext> executionProcessContext() const;
  struct InheritedExecutionContext {
    uint64_t ThreadKey;
    uint32_t UserProcessID;
    bool UserMemoryAuthority;
    std::optional<ExecutionProcessContext> Process;
  };
  std::map<uint64_t, InheritedExecutionContext> InheritedExecutionContexts;
  struct ApcState {
    uint16_t CriticalDepth = 0;
    uint16_t GuardedDepth = 0;
  };
  std::map<uint64_t, ApcState> ApcStates;
  enum class ApcRegionKind { Critical, Guarded };
  llvm::Expected<ApcState *> apcStateForCall();
  llvm::Expected<uint64_t> apcsDisabled();
  llvm::Expected<uint64_t> allApcsDisabled();
  llvm::Expected<uint64_t> enterApcRegion(ApcRegionKind Kind);
  llvm::Expected<uint64_t> leaveApcRegion(ApcRegionKind Kind);
  bool UserRequestContext = false;
  uint32_t CurrentUserProcessID = 0;
  uint64_t NextUserAddress = profile::UserArenaBase;
  struct UserAllocation {
    uint64_t Size;
    uint32_t ProcessID;
    DriverUserPageAccess Access;
  };
  std::map<uint64_t, UserAllocation> UserAllocations;
  std::set<uint64_t> RevokedUserAllocations;
  std::set<uint32_t> ExitedUserProcesses;
  struct UserMdlView {
    uint64_t MDL = 0;
    uint64_t Address = 0;
    uint64_t PageBase = 0;
    uint64_t MappedSize = 0;
    uint64_t BackingAddress = 0;
    uint32_t ProcessID = 0;
    uint32_t Length = 0;
    unsigned Permissions = 0;
  };
  using UserMdlViewKey = std::pair<uint32_t, uint64_t>;
  std::map<UserMdlViewKey, UserMdlView> UserMdlViews;
  std::map<uint32_t, uint64_t> ProcessObjects;
  std::map<uint64_t, uint32_t> ProcessObjectIDs;
  struct ProcessAttachment {
    uint64_t Execution;
    uint64_t ApcState;
    uint32_t PreviousProcessID;
    bool PreviousUserContext;
    bool Private = false;
  };
  std::vector<ProcessAttachment> ProcessAttachments;
  bool hasProcessAttachment(uint64_t Execution) const;
  struct ExecutiveSpinLock {
    uint64_t Execution;
    uint8_t OldIRQL;
    bool RaisedIRQL;
  };
  std::map<uint64_t, ExecutiveSpinLock> ExecutiveSpinLocks;
  enum class SpinLockMode { Raise, AtDpc, TryAtDpc };
  llvm::Error validateSpinLockAddress(uint64_t Address) const;
  llvm::Error validateSpinLockStorage(uint64_t Address) const;
  llvm::Expected<uint64_t> initializeSpinLock(uint64_t Address);
  llvm::Expected<uint64_t> acquireSpinLock(uint64_t Address, SpinLockMode Mode);
  llvm::Expected<uint64_t> releaseSpinLock(uint64_t Address,
                                           std::optional<uint8_t> RestoreIRQL);
  // In-stack queued spin locks store the caller's KLOCK_QUEUE_HANDLE pointer in
  // the lock and the saved IRQL in the handle, keyed here by the lock address.
  struct QueuedSpinLock {
    uint64_t Execution;
    uint64_t Handle;
    uint8_t OldIRQL;
    bool RaisedIRQL;
  };
  std::map<uint64_t, QueuedSpinLock> QueuedSpinLocks;
  // A lock address or an execution holds a lock if either spin-lock kind tracks
  // it, so every held-lock rule treats executive and queued locks alike.
  bool spinLockHeldAt(uint64_t Address) const {
    return ExecutiveSpinLocks.count(Address) || QueuedSpinLocks.count(Address);
  }
  bool executionHoldsSpinLock(uint64_t Execution) const {
    for (const auto &[Address, Lock] : ExecutiveSpinLocks)
      if (Lock.Execution == Execution)
        return true;
    for (const auto &[Address, Lock] : QueuedSpinLocks)
      if (Lock.Execution == Execution)
        return true;
    return false;
  }
  llvm::Error validateQueueHandle(uint64_t Handle) const;
  llvm::Expected<uint64_t> acquireQueuedSpinLock(uint64_t SpinLock,
                                                 uint64_t Handle, bool Raise);
  llvm::Expected<uint64_t> releaseQueuedSpinLock(uint64_t Handle, bool Restore);
  // Run-down protection keeps its whole state in the caller's EX_RUNDOWN_REF
  // Count word, so these operations only validate and update guest memory.
  llvm::Expected<uint64_t> readRundownCount(uint64_t RunRef);
  llvm::Expected<uint64_t> initializeRundownProtection(uint64_t RunRef);
  // \p Units is 1 for the plain routines and the ULONG count of the Ex forms.
  llvm::Expected<uint64_t> acquireRundownProtection(uint64_t RunRef,
                                                    uint32_t Units);
  llvm::Expected<uint64_t> releaseRundownProtection(uint64_t RunRef,
                                                    uint32_t Units);
  llvm::Expected<uint64_t> waitForRundownProtectionRelease(uint64_t RunRef);
  llvm::Expected<uint64_t> rundownCompleted(uint64_t RunRef);
  llvm::Error validateInterlockedLock(uint64_t Lock) const;
  llvm::Error validateInterlockedStorage(uint64_t Lock, uint64_t Address,
                                         uint32_t Size, bool IsWrite) const;
  llvm::Expected<uint64_t> readInterlockedLink(uint64_t Lock, uint64_t Address,
                                               bool IsWrite) const;
  llvm::Expected<uint64_t> interlockedInsertList(llvm::ArrayRef<uint64_t> A,
                                                 bool Tail);
  llvm::Expected<uint64_t>
  interlockedRemoveHeadList(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> interlockedPushEntryList(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> interlockedPopEntryList(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> interlockedAdd(llvm::ArrayRef<uint64_t> A,
                                          unsigned Size);
  llvm::Error validateSListStorage(uint64_t Address, uint32_t Size,
                                   bool IsWrite) const;
  llvm::Expected<std::pair<uint64_t, uint64_t>>
  readSListHeader(uint64_t Header, bool IsWrite) const;
  llvm::Expected<uint64_t> initializeSList(uint64_t Header);
  llvm::Expected<uint64_t> pushEntrySList(uint64_t Header, uint64_t Entry);
  llvm::Expected<uint64_t> popEntrySList(uint64_t Header);
  llvm::Expected<uint64_t> flushSList(uint64_t Header);
  llvm::Expected<uint64_t> querySList(uint64_t Header, bool FirstEntry);

  struct Lookaside {
    bool Paged;
    uint32_t Type, EntrySize, Tag;
    uint64_t Allocate, Free;
  };
  std::map<uint64_t, Lookaside> Lookasides;
  uint64_t LookasideHeads[2] = {};
  llvm::Expected<uint64_t> initializeLookaside(llvm::ArrayRef<uint64_t> A,
                                               bool Paged);
  llvm::Expected<uint64_t> deleteLookaside(uint64_t Address, bool Paged);
  llvm::Error validateLookasideAccess(uint64_t Address, uint32_t Size,
                                      bool IsWrite) const;
  llvm::Error canReleaseLookasideRange(uint64_t Address, uint64_t Size,
                                       uint64_t Retiring) const;
  llvm::Expected<std::vector<uint64_t>>
  lookasideEntries(uint64_t Address) const;
  llvm::Error writableLookasideStorage(uint64_t Address, uint32_t Size) const;
  llvm::Error validateKernelObjectBorrow(uint64_t Address, uint64_t Size) const;
  llvm::Error validateUnborrowedKernelStorage(uint64_t Address,
                                              uint64_t Size) const;
  enum class FastMutexMode { Normal, Unsafe, UnsafeGuarded };
  struct FastMutexHold {
    uint64_t Thread, Object;
    uint8_t OldIRQL;
    FastMutexMode Mode;
  };
  std::map<uint64_t, FastMutexHold> FastMutexHolds;
  llvm::Expected<uint64_t> initializeFastMutex(uint64_t Address);
  llvm::Expected<uint64_t> acquireFastMutex(uint64_t Address,
                                            FastMutexMode Mode, bool Try);
  llvm::Expected<uint64_t> releaseFastMutex(uint64_t Address, bool Unsafe);
  llvm::Error validateFastMutexStorage(uint64_t Address, bool Initialize) const;
  llvm::Error validateFastMutexAccess(uint64_t Address, uint32_t Size,
                                      bool IsWrite) const;
  llvm::Error validateFastMutexContext(uint8_t IRQL, uint16_t Critical,
                                       uint16_t Guarded,
                                       uint64_t Retiring = 0) const;
  bool threadHoldsFastMutex(uint64_t Thread) const;
  enum class ExecutiveResourceMode {
    Exclusive,
    Shared,
    SharedWaitForExclusive
  };
  struct ExecutiveResourceOwner {
    uint64_t Thread = 0, Value = 0;
    uint32_t Count = 0;
  };
  struct ExecutiveResource {
    uint64_t Flink = 0, Blink = 0, Table = 0;
    bool Exclusive = false, BoostDisabled = false;
    // Slot zero is the inline owner. A table has an unused header slot zero
    // and the remaining owners at the same indices as this vector.
    std::vector<ExecutiveResourceOwner> Owners{1};
  };
  std::map<uint64_t, ExecutiveResource> ExecutiveResources;
  uint64_t ExecutiveResourceHead = 0;
  llvm::Error validateExecutiveResourceAccess(uint64_t Address,
                                              uint64_t Size) const;
  llvm::Error canReleaseExecutiveResourceRange(uint64_t Address,
                                               uint64_t Size) const;
  llvm::Error validateExecutiveResourceMemory(uint64_t Address, uint32_t Size,
                                              bool Write) const;
  llvm::Expected<ExecutiveResource *>
  executiveResource(uint64_t Address, bool Write,
                    bool AllowTransferred = false);
  std::vector<uint8_t> executiveResourceBytes(const ExecutiveResource &State,
                                              bool Table) const;
  llvm::Error validateExecutiveResourceRegistry() const;
  llvm::Error publishExecutiveResource(uint64_t Address,
                                       const ExecutiveResource &State);
  llvm::Error validateExecutiveResourceContext(uint8_t IRQL, uint16_t Critical,
                                               uint16_t Guarded,
                                               uint64_t Retiring = 0,
                                               uint64_t Owner = 0,
                                               bool IgnoreMutex = false) const;
  llvm::Error validateExecutiveResourceRestoreIRQL(uint8_t IRQL) const;
  bool threadHoldsExecutiveResource(uint64_t Thread) const;
  llvm::Expected<uint64_t> initializeExecutiveResource(uint64_t Address);
  llvm::Expected<uint64_t> resetExecutiveResource(uint64_t Address,
                                                  bool Delete);
  llvm::Expected<uint64_t> acquireExecutiveResource(uint64_t Address,
                                                    ExecutiveResourceMode Mode,
                                                    bool Wait,
                                                    bool EnterCritical = false);
  llvm::Expected<uint64_t> releaseExecutiveResource(uint64_t Address,
                                                    uint64_t Owner,
                                                    bool LeaveCritical);
  llvm::Expected<uint64_t> queryExecutiveResource(uint64_t Address,
                                                  bool Exclusive, bool Waiters);
  llvm::Expected<uint64_t> convertExecutiveResource(uint64_t Address);
  llvm::Expected<uint64_t> disableExecutiveResourceBoost(uint64_t Address);
  llvm::Expected<uint64_t>
  transferExecutiveResource(uint64_t Address, uint64_t Owner, uint32_t Flags);
  struct RaisedIRQL {
    uint64_t Execution;
    uint8_t OldIRQL;
    uint8_t NewIRQL;
  };
  std::vector<RaisedIRQL> RaisedIRQLs;
  llvm::Expected<uint64_t> raiseIRQL(uint8_t RequestedIRQL);
  llvm::Expected<uint64_t> lowerIRQL(uint8_t RequestedIRQL);
  llvm::Expected<uint64_t> processObject(uint32_t ProcessID);
  llvm::Expected<uint64_t> requestorProcess(uint64_t IRP);
  llvm::Expected<uint64_t> currentProcess();
  llvm::Error stackAttachProcess(uint64_t Process, uint64_t ApcState,
                                 bool Private = false);
  llvm::Error unstackDetachProcess(uint64_t ApcState, bool Private = false);
  llvm::Error probeAndLockProcessPages(uint64_t MDL, uint64_t Process,
                                       uint8_t Mode, uint32_t Operation);
  llvm::Expected<uint64_t> allocateUserBuffer(uint32_t Size,
                                              llvm::ArrayRef<uint8_t> Initial,
                                              DriverUserPageAccess Access,
                                              uint32_t ProcessID);
  llvm::Expected<uint64_t> probeUserBuffer(uint64_t Address, uint64_t Size,
                                           uint32_t Alignment, bool ForWrite);
  std::optional<KernelGuestCall> PendingInterruptCall;
  std::map<uint64_t, uint64_t> FrameworkInterruptContinuations;
  std::optional<uint64_t> EnteringPassiveInterrupt;
  std::map<uint64_t, uint64_t> PassiveInterruptThreads;
  std::map<std::pair<uint64_t, uint64_t>, uint8_t> FrameworkInterruptLocks;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> FrameworkPassiveLockThreads;
  std::map<uint64_t, bool> FrameworkLockStorage;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> FrameworkWaitLockThreads;
  struct InterruptParameters {
    uint64_t Record = 0;
    uint64_t Output = 0;
    uint64_t Routine = 0;
    uint64_t Context = 0;
    uint64_t SpinLock = 0;
    uint64_t PDO = 0;
    uint64_t Vector = 0;
    uint64_t IRQL = 0;
    uint64_t Synchronize = 0;
    uint64_t Mode = 0;
    uint64_t Share = 0;
    uint64_t Affinity = 0;
    uint64_t Floating = 0;
    uint64_t Group = 0;
    uint32_t Version = 0;
    bool LineBased = false;
    bool MessageBased = false;
    bool Passive = false;
    uint64_t Fallback = 0;
  };
  llvm::Expected<uint64_t> connectInterrupt(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> connectInterruptEx(uint64_t Record);
  llvm::Expected<uint64_t> registerInterrupt(InterruptParameters Parameters);
  llvm::Expected<uint64_t> disconnectInterrupt(uint64_t Object,
                                               uint32_t Version = 0);
  llvm::Expected<uint64_t> disconnectInterruptEx(uint64_t Record);
  llvm::Expected<uint64_t> acquireInterruptSpinLock(uint64_t Object);
  llvm::Expected<uint64_t> releaseInterruptSpinLock(uint64_t Object,
                                                    uint8_t OldIRQL);
  llvm::Expected<uint64_t>
  synchronizeInterrupt(uint64_t Object, uint64_t Routine, uint64_t Context);
  llvm::Expected<std::optional<uint64_t>> finishInterruptCall(uint64_t Token,
                                                              uint64_t Value);
  llvm::Expected<uint64_t> preflightScheduledBoundary(uint64_t Time);
  llvm::Error processScheduledBoundary(uint64_t Time, bool Executing);
  bool canServicePassiveEvents() const {
    return !InstructionClock ||
           (CurrentIRQL == scheduler::PassiveLevel && !CancelLock.Held);
  }
  llvm::Error processInterruptEvents();
  std::optional<uint64_t> nextInterruptEventTime() const {
    return Interrupts.nextEventTime();
  }
  llvm::Error canReleaseResources(uint64_t PDO) const;
  std::optional<Wait> PendingWait;
  uint64_t NextWaitRegistration = kernel_wait::FirstRegistration;
  std::map<uint64_t, Wait> WaitRegistrations;
  std::map<uint64_t, size_t> WaitReferences;
  std::map<std::pair<uint64_t, uint32_t>, size_t> WaitBlockReferences;
  enum class ObjectKind {
    Process,
    Thread,
    Driver,
    File,
    Event,
    Semaphore,
    Job,
    Token,
    Device,
    Directory,
    SymbolicLink,
    Key
  };
  struct ObjectReferences {
    ObjectKind Kind;
    size_t Pointers = 0;
    size_t Handles = 0;
    std::string Name;
    KernelRegistry *RegistryOwner = nullptr;
  };
  struct ObjectHandle {
    uint64_t Object;
    uint32_t GrantedAccess;
    uint32_t Attributes;
    uint32_t ProcessID;
    bool ScenarioOwned = false;
  };
  // External references have one owner. Execution and registered waits keep
  // their domain-owned holds until completion; they are not caller references.
  std::map<uint64_t, ObjectReferences> ReferencedObjects;
  std::map<ObjectKind, uint64_t> ObjectTypes;
  std::map<uint64_t, ObjectHandle> ObjectHandles;
  uint64_t NextObjectHandle = profile::SystemThreadHandleBase;
  struct ThreadObject {
    uint64_t CallbackID = 0;
    uint32_t ProcessID = 0;
    uint32_t ExitStatus = 0;
    uint32_t ExitPriority = 0;
    bool System = false;
    bool Exited = false;
    bool Terminating = false;
  };
  std::map<uint64_t, ThreadObject> ThreadObjects;
  std::map<uint64_t, uint64_t> CurrentThreadObjects;
  std::optional<uint32_t> PendingThreadTermination;
  /// Client IDs of live thread objects, assigned on first observation. Issued
  /// IDs are never reused, and requestor process IDs may not collide with them.
  std::map<uint64_t, uint32_t> ThreadClientIDs;
  std::set<uint32_t> IssuedThreadClientIDs;
  std::set<uint32_t> RequestorProcessIDs;
  uint64_t NextThreadClientID = profile::ThreadIDBase;
  llvm::Expected<uint64_t> threadClientID(uint64_t Object);
  llvm::Expected<uint64_t>
  createSystemThread(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> terminateSystemThread(uint32_t Status);
  llvm::Expected<uint64_t>
  referenceObjectByHandle(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  referenceObjectByPointer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> referenceObject(uint64_t Object);
  llvm::Expected<uint64_t> dereferenceObject(uint64_t Object);
  llvm::Expected<uint64_t> objectType(uint64_t Object);
  llvm::Expected<uint64_t> objectType(ObjectKind Kind);
  llvm::Error initializeDataExports(const DriverOptions &Options);
  llvm::Error initializeObjectNamespace();
  struct ObjectLookup {
    uint64_t Status;
    uint64_t Object = 0;
    std::string RemainingName;
  };
  llvm::Expected<ObjectLookup>
  lookupNamedObject(llvm::StringRef Name, uint64_t Root, uint32_t Attributes,
                    bool OpenLink, bool ParseDevice = false,
                    unsigned LinkDepth = 0);
  std::string objectName(uint64_t Object) const;
  uint64_t findSymbolicLink(llvm::StringRef Key) const;
  const ObjectHandle *findObjectHandle(uint64_t Handle) const;
  static std::optional<uint32_t> objectAccessMask(ObjectKind Kind);
  static llvm::Expected<uint32_t> mapObjectAccess(ObjectKind Kind,
                                                  uint32_t Access);
  llvm::Expected<uint64_t> referenceObjectByName(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> openNamedObject(llvm::ArrayRef<uint64_t> A,
                                           ObjectKind Kind);
  llvm::Expected<uint64_t> queryObjectName(uint64_t Object, uint64_t Buffer,
                                           uint32_t Length,
                                           uint64_t ReturnLength,
                                           bool RequiredLength);
  llvm::Expected<uint64_t> queryObject(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> querySymbolicLink(llvm::ArrayRef<uint64_t> A);
  llvm::Error validateDataExportAccess(uint64_t Address, uint32_t Size,
                                       bool IsWrite) const;
  bool DebuggerStateDeclared = false;
  llvm::Expected<uint64_t>
  openObjectByPointer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> lookupProcess(uint64_t ID, uint64_t Output);
  llvm::Expected<uint64_t> lookupThread(uint64_t ID, uint64_t Output);
  llvm::Expected<uint64_t> threadProcess(uint64_t Object);
  llvm::Expected<uint64_t> threadTerminating(uint64_t Object) const;
  llvm::Error validateObjectOutput(uint64_t Address, uint32_t Size);
  bool matchesObjectType(ObjectKind Kind, uint64_t Type) const;
  void retainObjectHandle(uint64_t Object, uint32_t Access, uint32_t Attributes,
                          uint32_t ProcessID);
  llvm::Expected<uint64_t> closeHandle(uint64_t Handle);
  void retireObjectIfUnreferenced(uint64_t Object);
  void retireBorrowedThread(uint64_t Key);
  llvm::Expected<uint64_t> threadPriorityKey(uint64_t Object,
                                             bool Changing) const;
  llvm::Expected<uint64_t> queryThreadPriority(uint64_t Object) const;
  llvm::Expected<uint64_t> setThreadPriority(uint64_t Object, int32_t Priority);
  // Canonical logical-thread ownership survives parked and nested executions.
  std::set<uint64_t> SystemAffinityThreads;
  llvm::Expected<uint64_t> currentProcessorNumber(uint64_t Output);
  llvm::Expected<uint64_t> setSystemAffinity(uint64_t Mask);
  llvm::Expected<uint64_t> revertSystemAffinity();
  std::map<uint64_t, size_t> RemoveLockWaitReferences;
  llvm::Expected<uint64_t>
  initializeRemoveLock(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  acquireRemoveLock(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> releaseRemoveLock(llvm::ArrayRef<uint64_t> Arguments,
                                             bool Wait);
  llvm::Error validateRemoveLockOwner(uint64_t Lock) const;
  llvm::Error canReleaseRemoveLockStorage(uint64_t Base, uint64_t Size) const;
  llvm::Error canReleaseRange(uint64_t Base, uint64_t Size,
                              uint64_t IgnoredDMAPin = 0,
                              uint64_t RetiringLookaside = 0) const;
  llvm::Expected<uint64_t> beginWait(llvm::ArrayRef<uint64_t> Arguments,
                                     bool Delay);
  llvm::Expected<uint64_t>
  beginMultipleWait(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> beginObjectWait(Wait Pending, uint64_t Timeout);
  llvm::Expected<std::optional<uint32_t>>
  acquireWaitObjects(const Wait &Pending);
  llvm::Error releaseWaitReferences(const Wait &Pending);
  llvm::Error
  canRevokeVirtualRange(uint64_t Base, uint64_t Size,
                        std::optional<UsbIdleKey> RetiringUsbIdle = {},
                        uint64_t RetiringLookaside = 0) const;
  llvm::Error prepareRevokeVirtualRange(uint64_t Base, uint64_t Size);
  llvm::Error prepareReleaseRange(uint64_t Base, uint64_t Size,
                                  uint64_t IgnoredDMAPin = 0);
  llvm::Error
  prepareReleaseRanges(llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Ranges,
                       uint64_t IgnoredDMAPin = 0);
  llvm::Error validateGuestAccessImpl(uint64_t Address, uint32_t Size,
                                      bool IsWrite,
                                      bool IncludeDispatcher) const;
  llvm::Error validateDispatcherStorage(uint64_t Address, uint32_t Size,
                                        bool IsWrite) const;
  uint8_t CurrentIRQL = 0;
  bool InstructionClock = false;
  struct CancelSpinLockState {
    bool Held = false;
    bool Callback = false;
    uint64_t Owner = 0;
    uint64_t CallbackExecution = 0;
    uint64_t IRP = 0;
    uint8_t OldIRQL = 0;
    uint8_t CallbackIRQL = 0;
  } CancelLock;
  std::map<uint64_t, uint64_t> WorkItems;
  std::map<uint64_t, uint64_t> WorkReferences;
  std::map<uint64_t, GuestCallToken> ScheduledModelContinuations;
  llvm::Error processRequestCancellations();
  llvm::Expected<uint64_t> acquireCancelSpinLock(uint64_t OldIRQL);
  llvm::Error releaseCancelSpinLock(uint64_t OldIRQL);
  llvm::Expected<uint64_t> cancelIRP(uint64_t IRP);
  llvm::Expected<uint64_t> allocateWorkItem(uint64_t Device);
  llvm::Error queueWorkItem(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error freeWorkItem(uint64_t Address);
  llvm::Error updateDeviceReferences(uint64_t Device);
  llvm::Expected<uint64_t> resolveRoutine(uint64_t Address);
  llvm::Expected<uint64_t> querySystemInformation(llvm::ArrayRef<uint64_t> Args,
                                                  bool Trusted);
  llvm::Expected<uint64_t> pcToFileHeader(uint64_t PC, uint64_t Output);
  std::vector<KernelLoadedModule> LoadedModules;
  // Only loader-owned mapped spans, excluding image holes. Physical backing is
  // registered lazily when a permitted image range is first locked.
  std::map<uint64_t, uint64_t> ImageRAM;
  ExecutionContract Contract = ExecutionContract::Legacy;
  struct BugCheckRecord {
    uint64_t Callback, Component;
    uint32_t Reason;
    bool Live;
  };
  // Retain opaque record identities after removal: native list/checksum bytes
  // have no concrete model value, even when the State byte becomes empty.
  std::map<uint64_t, BugCheckRecord> BugCheckRecords;
  llvm::Error validateBugCheckRecord(uint64_t Record) const;
  llvm::Error validateBugCheckAccess(uint64_t Address, uint32_t Size,
                                     bool IsWrite) const;
  llvm::Error validateBugCheckPageBorrow(uint64_t Address, uint64_t Size) const;
  bool hasLiveBugCheckCallbacks() const;
  llvm::Expected<uint64_t> registerBugCheckCallback(uint64_t Record,
                                                    uint64_t Callback,
                                                    uint32_t Reason,
                                                    uint64_t Component);
  llvm::Expected<uint64_t> deregisterBugCheckCallback(uint64_t Record);
  std::optional<uint64_t> imageOwnerForRange(uint64_t Address,
                                             uint64_t Size) const;
  bool unpackImageMDLCall(KernelAPIKind Kind,
                          llvm::ArrayRef<uint64_t> Arguments) const;
  uint64_t DriverObject = 0;
  uint64_t RegistryPath = 0;
  uint64_t DriverExtension = 0;
  uint64_t DriverLoaderRecord = 0;
  bool EntryFinished = false;
  std::optional<std::map<uint64_t, std::vector<uint8_t>>> UnpackBaseline;
  bool UnpackOpaqueEffects = false;
  llvm::Error validateQueryBuffer(uint64_t Address, uint32_t Size,
                                  bool Writable, llvm::StringRef API);
  std::optional<DriverKernelVaShadowQuery> KernelVaShadowQuery;
  std::vector<DriverProcessPeb> ProcessPebs;
  llvm::Expected<uint64_t> queryKernelVaShadow(llvm::ArrayRef<uint64_t> Args);
  std::optional<DriverVersionQuery> VersionQuery;
  llvm::Expected<uint64_t> queryVersion(uint64_t Address);
  llvm::Expected<uint64_t>
  captureCallerContext(uint64_t Address, const KernelContextCapture *Context);
  std::optional<DriverPhysicalMemoryInventory> PhysicalMemoryInventory;
  llvm::Expected<uint64_t> queryPhysicalMemoryRanges();
  // Admission conservatively records reads that can expose model PFNs,
  // including reads performed by modeled copy/compare services.
  mutable bool UnpackMDLIdentityRead = false;
  uint64_t NextAllocation = 0;
  uint64_t AllocationEnd = 0;
  struct PoolAllocation {
    uint64_t Size;
    uint32_t Tag;
    bool NonPaged;
    // Kernel-returned buffers need pool lifetime checks but may have no
    // supported external allocation recipe for native reconstruction.
    std::optional<WindowsDriverState::PoolAPI> API;
    uint64_t TypeOrFlags;
  };
  std::map<uint64_t, PoolAllocation> Allocations;
  std::map<uint64_t, uint64_t> ArenaAllocations;
  enum class DeviceOwnerKind { Guest, PnpProvider, FileSystemProvider };
  struct DeviceRecord {
    uint64_t Address = 0;
    uint64_t Extension = 0;
    uint32_t Type = 0;
    std::string Name;
    uint64_t OwnerDriver = 0;
    DeviceOwnerKind OwnerKind = DeviceOwnerKind::Guest;
    // Association survives detach; it is not the current attachment edge.
    uint64_t PnpDevice = 0;
    uint64_t Lower = 0;
    uint64_t Upper = 0;
    uint64_t Size = 0;
    // Request and callback holds are distinct from open handles and from the
    // DEVICE_OBJECT.ReferenceCount field. Attachment links also prevent retire.
    uint64_t InternalReferences = 0;
    bool DeletePending = false;
    bool UnlinkedFromDriver = false;
    std::optional<DevicePowerState> ReportedDevicePower;
  };
  std::map<uint64_t, DeviceRecord> Devices;
  // PnP provider identities persist after the concrete PDO is retired.
  struct PnpDeviceRecord {
    uint64_t PDO = 0;
    // Devnode parent identity persists independently of attachment and
    // presence.
    uint64_t ParentPDO = 0;
    size_t ResultIndex = 0;
    DriverBusKind Bus = DriverBusKind::ResourceFree;
    std::optional<uint32_t> AddDeviceStatus;
    bool AddDeviceActive = false;
    bool FrameworkAdd = false;
    uint64_t FrameworkInit = 0;
    std::set<uint64_t> ExistingGuestDevices;
    std::set<uint64_t> GuestDevices;
    std::optional<DevicePowerState> InitialReportedDevicePower;
    std::vector<DriverPowerOperation> RequestedDevicePower;
    uint32_t RequestedPowerIndex = 0;
    // Identity of the last successfully committed START transaction.
    uint64_t StartEpoch = 0;
    std::optional<DriverWakeCapabilities> WakeCapabilities;
  };
  uint64_t PnpProviderDriver = 0;
  uint64_t FileSystemProviderDriver = 0;
  uint64_t deviceOwnerDriver(DeviceOwnerKind Kind) const;
  llvm::Error initializeFileSystem(const DriverOptions &Options);
  llvm::Error validateFileSystemRoute(
      llvm::ArrayRef<uint64_t> Route, uint64_t FileObject = 0,
      DriverRequestKind Kind = DriverRequestKind::DeviceControl) const;
  llvm::Expected<uint64_t> dispatchFileSystem(uint64_t Device, uint64_t IRP);
  bool PnpDevicesPrepared = false;
  std::vector<DriverPnpDevice> ConfiguredPnpDevices;
  std::map<std::string, PnpDeviceRecord> PnpDevices;
  DeviceLifecycle Lifecycle;
  bool isPnpProviderDevice(uint64_t Address) const;
  PnpDeviceRecord *pnpDeviceForPDO(uint64_t PDO);
  const PnpDeviceRecord *pnpDeviceForPDO(uint64_t PDO) const;
  llvm::Expected<uint64_t> pnpDeviceForRoute(uint64_t Device) const;
  llvm::Expected<uint64_t> deviceStartEpoch(uint64_t PDO,
                                            bool AllowStarting) const;
  llvm::Error validatePnpTopologyTransition(uint64_t PDO,
                                            DevicePnpRequest Minor) const;
  llvm::Error finishPnpRemoval(uint64_t PDO);
  llvm::Error retirePnpProvider(uint64_t PDO);
  llvm::Error snapshotPnpDevices();
  std::map<uint64_t, uint64_t> FreedRanges;
  std::array<bool, 28 * 8> DispatchBytesWritten{};
  std::shared_ptr<RAMInitializationWatch> DispatchInitialization;
  struct SymbolicLinkObject {
    std::string Target;
    std::vector<uint8_t> TargetBytes;
    bool Permanent = true;
    bool Named = true;
  };
  std::map<uint64_t, SymbolicLinkObject> SymbolicLinks;
  enum class FileState { Failed, Opening, Open, Cleaned, Closing };
  struct DeferredFileClose {
    DriverRequest Input;
    size_t ResultIndex;
    std::optional<size_t> SourceIndex;
  };
  struct OpenFile {
    enum class Origin { Scenario, DriverCreate };
    Origin Source = Origin::Scenario;
    uint64_t Address = 0;
    uint64_t Device = 0;
    /// Stable lifecycle owner even after the opened FDO is detached.
    uint64_t PnpDevice = 0;
    bool Asynchronous = false;
    FileState State = FileState::Opening;
    uint64_t ScenarioHandle = 0;
    uint32_t ScenarioID = 0;
    bool ScenarioCleanupRequested = false;
    uint32_t HandleCloseHolds = 0;
    uint32_t InternalIOHolds = 0;
    bool FileEventInitialized = false;
    uint64_t CreateOperation = 0;
    std::optional<uint64_t> ProviderOpen;
    uint64_t NameStorage = 0;
    uint64_t NameStorageSize = 0;
    bool OpenCancelled = false;
    uint64_t RelatedFile = 0;
    std::optional<DeferredFileClose> DeferredCleanup;
    std::optional<DeferredFileClose> DeferredClose;
  };
  bool owesFileClose(const OpenFile &File) const;
  void associateFileRequest(DriverRequestResult &Observation,
                            const OpenFile &File) const;
  llvm::Error validateFileSystemOpenBindings() const;
  // Scenario identifiers name an open instance, not its object-manager body.
  // A new CREATE may reuse an identifier while an older body remains held.
  std::map<uint32_t, uint64_t> Files;
  std::map<uint64_t, OpenFile> FileObjects;
  llvm::Expected<Invocation> beginFileRequest(const DriverRequest &Input,
                                              std::optional<size_t> SourceIndex,
                                              size_t Index,
                                              uint64_t RetainedFile = 0,
                                              uint64_t DispatchDevice = 0,
                                              uint64_t FileOperationID = 0);
  llvm::Error retireFileIfUnreferenced(
      uint64_t Object, std::optional<uint32_t> CallerProcess = std::nullopt);
  llvm::Error tryFinalizeFileClose(uint64_t IRP);
  llvm::Error waitForFileClose(uint64_t IRP);
  llvm::Expected<std::optional<uint64_t>>
  finishFileCloseDispatch(uint64_t Token, uint64_t ResultValue);
  std::set<uint64_t> InternalFileCloses;
  struct FileHandleClose {
    uint64_t File = 0;
    uint64_t IRP = 0;
    uint32_t CallerProcess = 0;
  };
  std::map<uint64_t, FileHandleClose> FileHandleCloses;
  uint64_t NextFileHandleClose = 1;
  llvm::Error validateFileHandleClose(uint64_t File, bool Scenario) const;
  llvm::Expected<std::optional<DriverRequestKind>>
  planFileHandleClose(uint64_t Handle) const;
  void releaseFileHandle(uint64_t Handle);
  llvm::Expected<Invocation> beginFileHandleRequest(uint64_t File,
                                                    DriverRequestKind Kind,
                                                    uint32_t ProcessID);
  llvm::Error closeFileHandle(uint64_t Handle);
  llvm::Error dispatchFileHandleClose(uint64_t ID, const Invocation &Call,
                                      uint32_t ProcessID);
  llvm::Error waitForFileHandleClose(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>>
  finishFileHandleDispatch(uint64_t Token, uint64_t ReturnValue);
  struct FileRequest {
    uint64_t File = 0, IRP = 0, ReturnedLength = 0, Storage = 0;
    uint64_t StatusOutput = 0;
    std::array<uint8_t, 16> StatusBytes{};
    uint64_t CallerExecution = 0, CallerThread = 0;
    uint32_t CallerProcess = 0;
    uint32_t Status = 0, Information = 0;
    bool Released = false;
    bool CompletionPublished = false;
    bool CriticalRegion = false;
    bool Native = false, PacketReleased = false, CopyStatusBlock = false;
  };
  std::map<uint64_t, FileRequest> FileRequests;
  uint64_t NextFileRequest = 1;
  std::optional<uint64_t> PendingFileRequestContinuation;
  enum class FileDeviceQuery { Base, Related };
  llvm::Expected<uint64_t> queryFileDevice(uint64_t File,
                                           FileDeviceQuery Query) const;
  llvm::Expected<uint64_t>
  beginFileAPIRequest(llvm::ArrayRef<uint64_t> A, DriverRequestKind Kind,
                      bool Native, const KernelContextCapture *Context);
  llvm::Expected<uint64_t>
  setFileInformation(llvm::ArrayRef<uint64_t> A,
                     const KernelContextCapture *Context);
  bool nativeFileRequest(uint64_t ID) const;
  llvm::Error releaseNativeFileRequestPacket(uint64_t IRP);
  llvm::Error validateFileAPIInterrupts(const KernelContextCapture *Context);
  llvm::Error publishFileRequestCompletion(uint64_t IRP);
  llvm::Expected<bool> fileRequestReady(uint64_t ID) const;
  llvm::Error waitForFileRequest(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>>
  finishFileRequestDispatch(uint64_t Token, uint64_t ReturnValue);
  struct FileOperation {
    bool CancelOpen = false;
    bool DevicePointer = false, PointerPublished = false,
         CriticalRegion = false;
    uint64_t File = 0, IRP = 0, Root = 0;
    uint64_t HandleOutput = 0, StatusOutput = 0, ReservedHandle = 0;
    uint64_t FileOutput = 0, DeviceOutput = 0;
    uint64_t Storage = 0, StorageSize = 0, AccessState = 0;
    uint64_t CallerExecution = 0, CallerThread = 0;
    uint32_t CallerProcess = 0, Access = 0, Attributes = 0;
    uint32_t Status = 0;
    uint64_t Information = 0;
    size_t ResultIndex = 0;
    bool Released = false, RootReleased = false, CompletionPublished = false;
  };
  std::map<uint64_t, FileOperation> FileOperations;
  uint64_t NextFileOperation = 1;
  std::optional<uint64_t> PendingFileOperation;
  enum class FileOpenKind { Create, Open, DevicePointer };
  llvm::Expected<uint64_t> createFile(llvm::ArrayRef<uint64_t> A,
                                      FileOpenKind Kind,
                                      const KernelContextCapture *Context);
  llvm::Expected<uint64_t>
  getDeviceObjectPointer(llvm::ArrayRef<uint64_t> A,
                         const KernelContextCapture *Context);
  llvm::Expected<uint64_t> cancelFileOpen(llvm::ArrayRef<uint64_t> A,
                                          const KernelContextCapture *Context);
  llvm::Expected<bool> fileOperationReady(uint64_t ID) const;
  llvm::Error waitForFileOperation(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>> resumeFileOperation(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>>
  finishFileOperationDispatch(uint64_t Token, uint64_t ReturnValue);
  llvm::Error startFileOperationDispatch(uint64_t ID, const Invocation &Call);
  llvm::Error prepareCancelFileOpenPacket(uint64_t IRP, uint64_t ID);
  llvm::Error publishFileOperationCompletion(uint64_t IRP);
  void retainReservedObjectHandle(uint64_t Handle, uint64_t Object,
                                  uint32_t Access, uint32_t Attributes,
                                  uint32_t ProcessID);
  bool Unloading = false;
  bool Unloaded = false;
  struct RequestedPower {
    uint64_t RequestDevice = 0;
    uint64_t Callback = 0;
    uint64_t Context = 0;
    // A separate callback input snapshot; the completed IRP stays retired.
    uint64_t StatusBlock = 0;
    uint32_t ResponseIndex = 0;
    std::optional<uint64_t> StartEpoch;
    bool CallbackStarted = false;
    bool CallbackReturned = false;
    DriverRequestOrigin Origin = DriverRequestOrigin::PoRequestPowerIrp;
    uint64_t FrameworkParent = 0;
    bool PrepareSystemSleep = false;
  };
  enum class PowerRequestDelivery { Inline, Queued };
  struct PowerRequestPlan {
    uint64_t PDO = 0, Top = 0, PC = 0;
    uint8_t StackCount = 0;
    DeviceLifecycleSnapshot Before;
    std::vector<uint64_t> Route;
    bool WaitWake = false, FrameworkPower = false;
    std::optional<uint64_t> StartEpoch;
  };
  struct UserRegion {
    DriverUserBufferKind Kind;
    std::string ID;
    uint64_t Address;
    uint32_t Size;
  };
  struct DriverIRPAllocation {
    uint64_t Size = 0;
    uint8_t StackCount = 0;
    bool Submitted = false;
    bool StorageReleased = false;
    bool CompletionHeld = false;
    uint32_t DispatchSlot = 0;
    uint64_t CompletionDevice = 0;
    std::optional<uint64_t> InitialDispatchToken;
    std::optional<uint32_t> ProviderDispatchReturn;
    std::optional<uint64_t> FreeCompletionToken;
    std::array<bool, 16> IOStatusWritten{};
    std::shared_ptr<RAMInitializationWatch> IOStatusInitialization;
  };
  std::map<uint64_t, DriverIRPAllocation> DriverIRPs;
  llvm::Expected<uint64_t>
  allocateDriverIRP(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error initializeIRPHeader(uint64_t IRP, uint8_t StackCount,
                                  uint8_t CurrentLocation);
  llvm::Error freeDriverIRP(uint64_t IRP);
  llvm::Error releaseDriverIRPStorage(uint64_t IRP);
  llvm::Error freeSubmittedDriverIRP(uint64_t IRP);
  llvm::Error adoptDriverIRP(uint64_t Device, uint64_t IRP);
  llvm::Error validateDriverIRPCompletion(uint64_t IRP, uint32_t Status,
                                          uint64_t Information) const;
  llvm::Error captureDriverIRPCompletion(uint64_t IRP);
  llvm::Error recordDriverIRPDispatchReturn(uint64_t IRP, uint32_t Status);
  llvm::Error tryFinalizeDriverIRP(uint64_t IRP);
  struct ActiveRequest {
    DriverRequestKind Kind;
    size_t ResultIndex;
    uint64_t IRP = 0;
    uint64_t Device = 0, FileAddress = 0;
    uint64_t PnpDevice = 0;
    std::optional<DeviceLifecycleTicket> PnpTicket;
    std::optional<DriverPnpOperation> PnpOperation;
    uint64_t RawResources = 0;
    uint64_t TranslatedResources = 0;
    uint64_t ResourceListSize = 0;
    std::optional<DeviceLifecycleTicket> PowerTicket;
    std::optional<DriverPowerOperation> PowerOperation;
    std::optional<DriverBusCompletion> FileBusCompletion;
    bool FileBusReceived = false;
    std::optional<RequestedPower> ChildPower;
    bool LifecycleIo = false;
    uint64_t Stack = 0;
    uint8_t StackCount = 1;
    /// Immutable route holds outlive packet completion until dispatch returns.
    std::vector<uint64_t> DeviceRoute;
    /// Historical pending bits captured when each slot was unwound.
    std::vector<std::optional<bool>> UnwoundPending;
    bool Forwarded = false;
    uint64_t SystemBuffer = 0;
    /// Internal query producers borrow caller storage through completion.
    bool BorrowedSystemBuffer = false;
    uint64_t FileRequestID = 0;
    uint64_t FileOperation = 0;
    bool FileSystemReceived = false;
    uint64_t UserBuffer = 0;
    uint64_t UserInput = 0;
    std::vector<UserRegion> UserRegions;
    uint64_t BufferSize = 0;
    uint64_t SecurityContext = 0;
    uint32_t OutputSize = 0;
    bool Completed = false;
    bool DispatchReturned = false;
    bool FrameworkRemoveStarted = false;
    bool FrameworkTransitionAwaiting = false;
    bool FrameworkTransitionBeforeBus = false;
    bool FrameworkTransitionHandled = false;
    bool FrameworkPolicyIssued = false;
    bool PendingMarked = false;
    bool CancelRequested = false;
    std::optional<uint64_t> CancelDeadline = std::nullopt;
    uint32_t FileId = 0;
    bool AsynchronousFile = false;
    uint32_t ProcessID = 0;
    uint32_t InputSize = 0;
    /// Effective process of an object-manager file lifecycle callback. The
    /// invoking thread's creating process and PreviousMode stay unchanged.
    std::optional<uint32_t> CallbackProcessID;
    uint32_t TransferSize = 0;
    uint64_t ByteOffset = 0;
    bool Direct = false;
    bool Neither = false;
    uint64_t Mdl = 0;
    uint64_t SystemMdl = 0;
    std::array<bool, 16> IOStatusWritten{};
    std::shared_ptr<RAMInitializationWatch> IOStatusInitialization;
  };
  std::map<uint64_t, ActiveRequest> Requests;
  bool canDeliverCancellation(const ActiveRequest &Request) const;
  llvm::Expected<std::optional<KernelScheduler::Callback>>
  planWDMCancellation(uint64_t IRP, const ActiveRequest &Request) const;
  llvm::Error
  validatePnpRemovalFinalization(const ActiveRequest &Request) const;
  std::set<uint64_t> FinalizedRequests;
  enum class IRPCallKind {
    Dispatch,
    Completion,
    PowerDispatch,
    PowerCompletion,
    Cancel,
    FileClose,
    FileHandleClose,
    FileRequest,
    FileOperation
  };
  struct IRPCall {
    IRPCallKind Kind;
    uint64_t IRP = 0;
    uint32_t Slot = 0;
    bool AwaitingCallback = false;
    uint64_t ReturnValue = 0;
    std::optional<uint64_t> ProviderReceiptIRP;
    std::optional<uint32_t> FrameworkDispatchStatus;
    uint64_t FileHandleOperation = 0;
    uint64_t FileRequestID = 0;
    uint64_t FileOperation = 0;
  };
  struct ProviderReceipt {
    uint64_t Device = 0;
    uint64_t Deadline = 0;
    std::vector<UsbIdleCompletionPlan> Idle;
    size_t NextIdle = 0;
    std::optional<uint64_t> ScheduledDispatchToken;
  };
  std::map<uint64_t, ProviderReceipt> ProviderReceipts;
  llvm::Expected<std::vector<UsbIdleCompletionPlan>>
  planUsbIdleReceipt(uint64_t PDO, uint64_t IRP) const;
  llvm::Expected<std::optional<uint64_t>> continueProviderReceipt(uint64_t IRP);
  llvm::Expected<uint64_t> completeProviderReceipt(uint64_t PDO, uint64_t IRP,
                                                   uint64_t Deadline);
  llvm::Expected<std::optional<uint64_t>>
  finishWdmGuestCallBody(uint64_t Token, uint64_t ResultValue);
  uint64_t NextIRPCall = 1;
  std::map<uint64_t, IRPCall> IRPCalls;
  std::optional<KernelGuestCall> PendingWdmCall;
  llvm::Expected<uint32_t> requestStackCursor(uint64_t IRP) const;
  llvm::Expected<uint64_t> currentRequestStack(uint64_t IRP) const;
  /// Only the framework host may forward a framework-owned file IRP. Guest
  /// WDM dispatch still requires independent ownership of the packet.
  enum class ForwardingOwner {
    WDM,
    FrameworkFile,
    FrameworkFileSynchronous,
    FrameworkFileAsynchronous,
    FrameworkFileAutomatic
  };
  llvm::Expected<uint64_t>
  callDriver(uint64_t Device, uint64_t IRP,
             ForwardingOwner Owner = ForwardingOwner::WDM,
             std::optional<int64_t> SendTimeout = std::nullopt);
  struct IRPCompletionStep {
    uint32_t Slot;
    bool Pending;
  };
  struct IRPCompletionPlan {
    uint32_t StartSlot = 0;
    std::vector<IRPCompletionStep> Steps;
    uint64_t PC = 0, Device = 0, Context = 0;
    bool PowerCompletion = false;
    std::vector<uint64_t> Arguments;
  };
  llvm::Expected<IRPCompletionPlan>
  planIRPCompletion(uint64_t IRP,
                    std::optional<uint32_t> StatusOverride = std::nullopt,
                    std::optional<bool> CancelOverride = std::nullopt) const;
  struct ProviderCompletion {
    enum class Kind {
      WDM,
      FrameworkCallback,
      FrameworkSynchronous,
      FrameworkAutomatic
    };
    uint64_t Device = 0, Deadline = 0, Sequence = 0;
    uint32_t Status = 0;
    Kind Owner = Kind::WDM;
  };
  uint64_t NextProviderSequence = 1;
  std::map<uint64_t, ProviderCompletion> ProviderCompletions;
  llvm::Expected<uint64_t>
  callProviderDriver(uint64_t Device, uint64_t IRP,
                     ForwardingOwner Owner = ForwardingOwner::WDM,
                     std::optional<int64_t> SendTimeout = std::nullopt);
  llvm::Error processProviderCompletions();
  llvm::Expected<std::optional<uint64_t>> advanceIRPCompletion(uint64_t Token);
  llvm::Expected<bool> dispatchPending(const ActiveRequest &Request,
                                       uint32_t Slot) const;
  llvm::Error validateCompletionPending(const ActiveRequest &Request,
                                        bool Pending) const;
  llvm::Error retireCompletedRequest(uint64_t IRP, uint8_t PriorityBoost);
  ActiveRequest *requestForIRP(uint64_t IRP);
  const ActiveRequest *requestForIRP(uint64_t IRP) const;
  void configureFrameworkRequestHost();
  llvm::Error markRequestPending(uint64_t IRP);
  llvm::Error prepareRequestBuffers(ActiveRequest &Record,
                                    const DriverRequest &Input);
  llvm::Error prepareUserRequestBuffers(ActiveRequest &Request,
                                        const DriverRequest &Input);
  llvm::Error snapshotUserBuffers();
  llvm::Expected<Invocation> beginPnpRequest(const DriverRequest &Input,
                                             size_t ResultIndex);
  llvm::Expected<Invocation> beginPowerRequest(const DriverRequest &Input,
                                               size_t ResultIndex);
  /// Prepare without calling the guest. A generated child uses the next result
  /// index and publishes its observation only after successful preparation.
  llvm::Expected<Invocation>
  preparePowerRequest(const DriverRequest &Input, size_t ResultIndex,
                      std::optional<RequestedPower> Child = std::nullopt);
  llvm::Expected<PowerRequestPlan>
  planPowerRequest(const DriverRequest &Input, size_t ResultIndex,
                   const std::optional<RequestedPower> &Child,
                   PowerRequestDelivery Delivery) const;
  llvm::Expected<Invocation>
  commitPowerRequest(const DriverRequest &Input, size_t ResultIndex,
                     std::optional<RequestedPower> Child,
                     PowerRequestPlan Plan);
  llvm::Expected<uint32_t> dispatchPreparedPowerRequest(const Invocation &Call);
  llvm::Expected<std::optional<KernelScheduler::Invocation>>
  dispatchScheduledPowerProvider(const KernelScheduler::Invocation &Call);
  llvm::Expected<bool> beginFrameworkPowerPolicy(uint64_t IRP);
  llvm::Expected<bool> canAllocatePowerRequest(const PowerRequestPlan &Plan,
                                               bool Callback) const;
  llvm::Expected<uint64_t> requestPowerIrp(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> setPowerState(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error startNextPowerIrp(uint64_t IRP);
  llvm::Error tryFinalizePowerRequest(uint64_t IRP);
  llvm::Expected<std::optional<uint64_t>> finishPowerCompletion(uint64_t Token);
  llvm::Error validatePowerRequestCompletion(const ActiveRequest &Request,
                                             uint32_t Status) const;
  llvm::Error finishRequestLifecycle(ActiveRequest &Request, uint32_t Status);
  llvm::Error initializePnpResources(ActiveRequest &Request);
  llvm::Error validatePnpRequestCompletion(const ActiveRequest &Request,
                                           uint32_t Status,
                                           bool ProviderProbe = false) const;
  llvm::Error publishProviderHardware(ActiveRequest &Request, uint32_t Status);
  llvm::Error initializeRequestPacket(ActiveRequest &Record,
                                      const DriverRequest &Input);
  struct LockedMdl {
    // Descriptor storage is independent of the described pages and their lock.
    enum class StorageKind { PrivateArena, NonPagedPool, ExecutionStack };
    StorageKind Storage = StorageKind::PrivateArena;
    uint64_t StorageOwner = 0;
    enum class Ownership {
      Request,
      RequestSystemBuffer,
      Driver,
      NonPagedPool,
      UserLocked,
      KernelLocked,
      AllocatedPages,
      ReleasedPages,
      Partial
    };
    Ownership Owner = Ownership::Request;
    uint64_t OwnerIRP = 0;
    uint64_t Address = 0;
    uint64_t Size = 0;
    uint64_t Buffer = 0;
    uint64_t OriginalAddress = 0;
    uint64_t BackingAddress = 0;
    uint64_t LockOwnerMDL = 0;
    uint64_t SystemMappingOwnerMDL = 0;
    bool OwnsSystemMapping = false;
    uint64_t AllocationSize = 0;
    uint64_t UserAddress = 0;
    uint32_t ByteCount = 0;
    struct AdvanceRange {
      uint64_t OriginalAddress, BackingAddress, MappingBase;
      uint32_t ByteCount, DroppedPages;
    };
    // Advancing the data window does not retire the original pin or alias.
    std::optional<AdvanceRange> Advance;
    uint64_t systemMappingBase() const;
    uint64_t Pool = 0;
    uint64_t Pin = 0;
    bool Writable = false;
    bool DmaWritable = false;
    bool Mapped = false;
    bool isProbeLocked() const {
      return Owner == Ownership::UserLocked || Owner == Ownership::KernelLocked;
    }
  };
  std::map<uint64_t, LockedMdl> MDLs;
  llvm::Error initializeMDLPhysicalPages(const LockedMdl &State);
  llvm::Expected<uint64_t> allocateMDL(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> createMDL(uint64_t Record, uint64_t Address,
                                     uint64_t Size);
  llvm::Error initializeMDLRecord(uint64_t Record, uint64_t Address,
                                  uint32_t Size, uint64_t Extent,
                                  uint16_t Flags);
  llvm::Error canReleaseMDLStorage(uint64_t Base, uint64_t Size) const;
  void retireMDLStorage(uint64_t Base, uint64_t Size);
  llvm::Expected<uint64_t>
  allocatePagesForMDL(llvm::ArrayRef<uint64_t> Arguments, bool Extended);
  llvm::Error freePagesFromMDL(uint64_t MDL);
  llvm::Error freeAllocatedMDL(uint64_t MDL);
  static llvm::Expected<KernelPhysicalMemory::CacheType>
  memoryCacheType(uint32_t Value);
  llvm::Error buildNonPagedMDL(uint64_t MDL);
  llvm::Error buildPartialMDL(uint64_t Source, uint64_t Target,
                              uint64_t Address, uint32_t Length);
  llvm::Error canReleaseMDLDependencies(uint64_t MDL,
                                        llvm::ArrayRef<uint64_t> Retiring = {},
                                        bool ReleasePages = true) const;
  llvm::Error probeAndLockPages(uint64_t MDL, uint32_t Mode,
                                uint32_t Operation);
  llvm::Error unlockPages(uint64_t MDL);
  llvm::Expected<uint64_t> advanceMDL(uint64_t MDL, uint32_t Bytes);
  llvm::Error restoreAdvancedMDL(LockedMdl &State);
  llvm::Expected<uint64_t> protectMDLSystemAddress(uint64_t MDL,
                                                   uint32_t Protection);
  llvm::Error freeMDL(uint64_t MDL);
  llvm::Expected<uint64_t> createMDLRecord(uint64_t Address, uint32_t Size,
                                           uint16_t Flags);
  llvm::Expected<uint64_t> frameworkRequestMDL(uint64_t IRP, bool Output);
  llvm::Expected<KernelFramework::LockedUserBuffer>
  frameworkProbeAndLockUserBuffer(uint64_t IRP, uint64_t Buffer,
                                  uint64_t Length, bool ForWrite);
  llvm::Error releaseFrameworkUserBuffer(uint64_t MDL);
  llvm::Expected<uint64_t> createRequestMDL(uint64_t IRP, uint32_t Size,
                                            llvm::ArrayRef<uint8_t> Initial,
                                            bool Writable, uint64_t UserAddress,
                                            bool DmaWritable);
  llvm::Expected<uint64_t>
  mapLockedPages(uint64_t MDL, uint32_t Priority, bool ReuseExisting,
                 KernelPhysicalMemory::CacheType Cache =
                     KernelPhysicalMemory::CacheType::Cached);
  llvm::Expected<uint64_t> mapUserMDL(uint64_t MDL, uint64_t RequestedAddress,
                                      uint32_t Priority,
                                      KernelPhysicalMemory::CacheType Cache);
  llvm::Error unmapUserMDL(uint64_t Address, uint64_t MDL);
  llvm::Error canReleaseMdlUserViews(uint64_t MDL) const;
  llvm::Error canReleaseUserViewsForBacking(uint64_t Address,
                                            uint64_t Size) const;
  llvm::Error validateUserMdlViewAccess(uint64_t Address, uint64_t Size,
                                        bool IsWrite) const;
  struct UserMemoryRange {
    uint64_t Owner;
    uint64_t Offset;
    uint64_t Backing;
  };
  llvm::Expected<UserMemoryRange> resolveUserMemoryRange(uint64_t Address,
                                                         uint64_t Length,
                                                         bool ForWrite) const;
  llvm::Error unmapLockedPages(uint64_t Address, uint64_t MDL);
  llvm::Error validateMDLAccess(uint64_t Address, uint32_t Size,
                                bool IsWrite) const;
  llvm::Expected<std::vector<uint64_t>> requestMDLChain(uint64_t IRP) const;
  llvm::Expected<std::vector<std::pair<uint64_t, uint64_t>>>
  requestReleaseRanges(uint64_t IRP) const;
  llvm::Error appendRequestMDLReleaseResources(
      uint64_t IRP, std::vector<std::pair<uint64_t, uint64_t>> &Ranges,
      std::vector<uint64_t> &Pins,
      std::vector<std::pair<uint64_t, uint64_t>> &RevokingAliases) const;
  llvm::Error expireRequestMDL(uint64_t IRP);
  llvm::Expected<std::vector<uint8_t>> readMDLBytes(uint64_t MDL,
                                                    uint32_t Count);
  llvm::Error completeRequest(uint64_t IRP, uint8_t PriorityBoost);
  llvm::Error validateRequestCompletion(uint64_t IRP, uint32_t Status,
                                        uint64_t Information) const;
  llvm::Error validateIOAccess(uint64_t Address, uint32_t Size, bool IsWrite,
                               bool DispatcherStorage = false) const;
  llvm::Expected<uint64_t> allocate(uint64_t Size, uint64_t Alignment = 16);
  llvm::Expected<uint64_t> isResidentKernelAddress(uint64_t Address) const;
  llvm::Expected<uint64_t> queryPhysicalAddress(uint64_t Address) const;
  llvm::Expected<uint64_t>
  queryPrimaryVirtualAddress(uint64_t PhysicalAddress) const;
  llvm::Expected<uint64_t> allocatePhysicalBuffer(uint64_t AllocationSize,
                                                  uint64_t Alignment,
                                                  uint64_t DataOffset,
                                                  uint64_t DataSize);
  llvm::Expected<uint64_t> makeUnicodeString(const std::string &Text);
  llvm::Expected<std::string>
  readObjectName(uint64_t Address,
                 size_t MaxCharacters = profile::MaxDeviceNameSize);
  llvm::Expected<uint64_t> createDevice(llvm::ArrayRef<uint64_t> Arguments);
  struct DeviceCreation {
    uint32_t Status;
    uint64_t Address;
  };
  llvm::Expected<DeviceCreation>
  createDeviceObject(llvm::StringRef Name, uint32_t ExtensionSize,
                     uint32_t Type, uint32_t Characteristics, bool Exclusive);
  llvm::Expected<DeviceCreation>
  createDeviceObjectForOwner(llvm::StringRef Name, uint32_t ExtensionSize,
                             uint32_t Type, uint32_t Characteristics,
                             bool Exclusive, uint64_t Owner,
                             DeviceOwnerKind OwnerKind);
  llvm::Expected<uint32_t> createSymbolicLink(llvm::StringRef Name,
                                              llvm::StringRef Target);
  llvm::Expected<uint32_t> deleteSymbolicLink(llvm::StringRef Name);
  llvm::Expected<std::string> linkKey(llvm::StringRef Name) const;
  llvm::Expected<uint64_t> resolveDeviceName(llvm::StringRef Name) const;
  llvm::Error deleteDevice(uint64_t Address);
  llvm::Error retireDeviceIfUnreferenced(uint64_t Address);
  llvm::Expected<uint64_t> topAttachedDevice(uint64_t Base) const;
  enum class DeviceStackQuery {
    Top,
    TopReference,
    LowerReference,
    BaseReference
  };
  llvm::Expected<uint64_t> queryDeviceStack(uint64_t Device,
                                            DeviceStackQuery Query);
  llvm::Expected<std::vector<uint64_t>> deviceStack(uint64_t Top) const;
  enum class ObjectRead { Execution, Snapshot };
  llvm::Error validateFileSystemDevice(uint64_t Address, ObjectRead Mode) const;
  llvm::Expected<uint64_t> readObjectField(uint64_t Address, unsigned Width,
                                           ObjectRead Mode) const;
  llvm::Error
  validateDeviceTopology(ObjectRead Mode = ObjectRead::Execution) const;
  llvm::Expected<std::vector<uint64_t>>
  driverDeviceInventory(ObjectRead Mode = ObjectRead::Execution) const;
  llvm::Expected<uint64_t> attachDevice(uint64_t Source, uint64_t Target);
  llvm::Error detachDevice(uint64_t Lower);
  llvm::Error retainDevice(uint64_t Device);
  llvm::Error releaseDevice(uint64_t Device);
  llvm::Error validateDeviceStackMutation(llvm::ArrayRef<uint64_t> Stack) const;
};
} // namespace neverd::emulation
#endif
