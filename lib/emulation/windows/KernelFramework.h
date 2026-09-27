//===- KernelFramework.h - Guest KMDF bindings and objects ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Session-owned KMDF identities, typed contexts and guest callback lifetimes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELFRAMEWORK_H
#define NEVERD_EMULATION_WINDOWS_KERNELFRAMEWORK_H

#include "../GuestMemory.h"
#include "KernelExportRegistry.h"
#include "KernelFrameworkPoFx.h"
#include "KernelGuestCall.h"
#include "KernelPowerPolicy.h"

#include "neverd/emulation/DriverPnp.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace neverd::emulation {
namespace framework {
namespace api {
#define NEVERD_FRAMEWORK_API(Name, Arity)                                      \
  constexpr llvm::StringLiteral Name = #Name;
#include "KernelFrameworkAPIs.def"
#undef NEVERD_FRAMEWORK_API
#define NEVERD_FRAMEWORK_LOADER_API(Name, Arity)                               \
  constexpr llvm::StringLiteral Name = #Name;
#include "KernelFrameworkLoaderAPIs.def"
#undef NEVERD_FRAMEWORK_LOADER_API
} // namespace api
#define NEVERD_FRAMEWORK_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelFrameworkInterruptValues.def"
#include "KernelFrameworkQueueValues.def"
#include "KernelFrameworkRequestValues.def"
#include "KernelFrameworkValues.def"
#undef NEVERD_FRAMEWORK_VALUE
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major)                      \
  constexpr uint32_t RequestMajor##Name = Major;
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
} // namespace framework

class KernelFramework {
public:
  using Allocate = std::function<llvm::Expected<uint64_t>(uint64_t)>;
  using Validate = std::function<llvm::Error(uint64_t, uint32_t, bool)>;
  using Release = std::function<llvm::Error(uint64_t, uint64_t)>;
  struct GuestCall {
    uint64_t Token = 0;
    uint64_t PC = 0;
    std::vector<uint64_t> Arguments;
    std::optional<GuestCallToken> ExecutionToken;
    uint64_t SynchronizationObject = 0;
  };
  struct DeviceCreation {
    uint32_t Status = 0;
    uint64_t Address = 0;
  };
  /// Typed bridge to the authoritative WDM device namespace and storage.
  /// Framework operations never fabricate a second DEVICE_OBJECT or API trace.
  struct DeviceHost {
    std::function<llvm::Expected<DeviceCreation>(llvm::StringRef, uint32_t,
                                                 bool)>
        Create;
    std::function<llvm::Expected<DeviceCreation>(
        uint64_t, llvm::StringRef, uint32_t, uint32_t, bool, bool)>
        CreatePnp;
    std::function<llvm::Error(uint64_t)> Delete;
    std::function<llvm::Error(uint64_t)> FinishInitializing;
    std::function<llvm::Error(const GuestCall &, uint64_t, uint8_t)> DeferCall;
    std::function<bool(uint64_t)> OwnsCallbackLock;
    std::function<llvm::Error(uint64_t)> AcquireCallbackLock;
    std::function<llvm::Error(uint64_t)> ReleaseCallbackLock;
    std::function<llvm::Expected<uint32_t>(uint64_t, llvm::StringRef)> Link;
  };
  void setDeviceHost(DeviceHost Host) { DevicesHost = std::move(Host); }
  struct InterruptSelection {
    uint64_t PDO = 0;
    uint32_t Ordinal = 0;
    std::optional<uint32_t> ResourceIndex;
    uint32_t MessageOrdinal = 0;
    bool Passive = false;
    bool CanWake = false;
    std::optional<bool> ShareVector;
    uint64_t SpinLock = 0, WaitLock = 0;
    uint8_t SynchronizeIRQL = 0;
  };
  struct InterruptConnection {
    uint64_t Token = 0, Affinity = 0;
    uint32_t Vector = 0, MessageID = 0, Polarity = 0, Mode = 0;
    uint8_t IRQL = 0, SynchronizeIRQL = 0;
    bool Message = false, Share = false;
  };
  /// WDM owns assigned resources, execution locks and interrupt delivery.
  struct InterruptHost {
    std::function<llvm::Expected<std::optional<InterruptConnection>>(
        const InterruptSelection &)>
        Describe;
    std::function<llvm::Expected<std::optional<InterruptConnection>>(
        const InterruptSelection &, uint64_t, uint64_t)>
        Connect;
    std::function<llvm::Error(uint64_t)> Disconnect;
    std::function<llvm::Error(uint64_t, bool)> SetActive;
    std::function<bool(uint64_t)> HasPendingWake;
    std::function<llvm::Expected<GuestCallToken>(
        uint64_t, uint64_t, llvm::ArrayRef<uint64_t>, uint64_t)>
        PrepareCall;
    std::function<llvm::Error(uint64_t)> Acquire;
    std::function<llvm::Error(uint64_t)> Release;
    std::function<llvm::Expected<bool>(uint64_t, uint64_t, uint64_t,
                                       llvm::ArrayRef<uint64_t>, bool, uint64_t,
                                       uint64_t)>
        QueueDeferred;
    std::function<bool(uint64_t)> HasDeferred;
  };
  void setInterruptHost(InterruptHost Host) {
    InterruptsHost = std::move(Host);
  }
  struct LockHost {
    std::function<llvm::Expected<uint64_t>(bool)> Create;
    std::function<llvm::Error(uint64_t, bool)> CanDelete;
    std::function<llvm::Error(uint64_t, bool)> Destroy;
    std::function<llvm::Expected<uint32_t>(uint64_t, bool,
                                           std::optional<int64_t>)>
        Acquire;
    std::function<llvm::Error(uint64_t, bool)> Release;
  };
  void setLockHost(LockHost Host) { LocksHost = std::move(Host); }
  struct RequestView {
    uint64_t IRP = 0, ByteOffset = 0;
    uint32_t Major = 0, ControlCode = 0, InputLength = 0, OutputLength = 0;
    bool Neither = false;
    uint64_t UserInput = 0, UserOutput = 0;
    uint64_t File = 0;
  };
  struct LockedUserBuffer {
    uint32_t Status = 0;
    uint64_t MDL = 0, Buffer = 0;
  };
  struct RequestHost {
    std::function<llvm::Expected<RequestView>(uint64_t)> View;
    std::function<llvm::Expected<uint64_t>(uint64_t, bool)> Buffer;
    std::function<llvm::Error(uint64_t)> MarkPending;
    std::function<llvm::Expected<bool>(uint64_t)> IsCanceled;
    std::function<llvm::Error(uint64_t)> RecordCancel;
    std::function<llvm::Expected<uint64_t>(uint64_t)> Information;
    std::function<llvm::Error(uint64_t, uint64_t)> SetInformation;
    /// Return the request-owned descriptor, or zero on allocation failure.
    std::function<llvm::Expected<uint64_t>(uint64_t, bool)> Mdl;
    std::function<llvm::Expected<LockedUserBuffer>(uint64_t, uint64_t, uint64_t,
                                                   bool)>
        ProbeAndLock;
    std::function<llvm::Error(uint64_t)> ReleaseUserBuffer;
    std::function<llvm::Error(uint64_t, uint32_t, uint64_t)> ValidateCompletion;
    std::function<llvm::Error(uint64_t, uint32_t, uint64_t)> Complete;
    /// Forward a file lifecycle IRP on its retained lower-device route. The
    /// WDM host owns the original packet and its terminal completion.
    std::function<llvm::Error(uint64_t)> ValidateFileForward;
    std::function<llvm::Expected<uint32_t>(uint64_t)> ForwardFile;
    /// Automatic forwarding retains the packet until file-object cleanup ends.
    std::function<llvm::Expected<uint32_t>(uint64_t)> ForwardFileAutomatically;
    /// A synchronous lower send returns its status while the framework keeps
    /// the original CREATE request for a later WdfRequestComplete.
    std::function<llvm::Expected<uint32_t>(uint64_t, std::optional<int64_t>)>
        SendFileSynchronously;
    /// An asynchronous send can retain the request until a later provider
    /// completion, even when the lower provider responds immediately.
    std::function<llvm::Expected<uint32_t>(uint64_t, std::optional<int64_t>)>
        SendFileAsynchronously;
  };
  void setRequestHost(RequestHost Host) { RequestsHost = std::move(Host); }
  struct RequestDispatch {
    uint64_t PC = 0;
    std::vector<uint64_t> Arguments;
    uint32_t Status = 0;
    bool CallerContext = false;
    uint64_t SynchronizationObject = 0;
  };
  /// Nullopt selects ordinary WDM dispatch; an engaged result owns the exact
  /// framework dispatch status independently from a void callback's RAX.
  llvm::Expected<std::optional<RequestDispatch>>
  routeRequest(uint64_t WdmDevice, uint64_t IRP, bool AfterCaller = false);
  llvm::Error finishRequestDispatch(uint64_t IRP);
  bool isPowerManagedCallback(uint64_t IRP, uint64_t Token) const;
  llvm::Expected<GuestCall>
  previewFileSendCompletion(uint64_t IRP, uint32_t Status,
                            uint64_t EarlierCallbacks = 0) const;
  llvm::Expected<GuestCall> queueFileSendCompletion(uint64_t IRP,
                                                    uint32_t Status,
                                                    uint64_t ReturnValue = 0);
  llvm::Error beginRequestCompletionCallback(uint64_t Token);
  llvm::Expected<std::optional<GuestCall>>
  previewAutomaticFileCompletion(uint64_t IRP, uint32_t Status) const;
  llvm::Expected<std::optional<GuestCall>>
  completeAutomaticFileForward(uint64_t IRP, uint32_t Status);
  bool isAutomaticFileContinuation(uint64_t Token) const;
  std::optional<bool> synchronousFileSendPending(uint64_t Request) const;
  llvm::Error validateSynchronousFileCompletion(uint64_t IRP,
                                                uint32_t Status) const;
  llvm::Error completeSynchronousFileSend(uint64_t IRP, uint32_t Status);
  llvm::Expected<RequestDispatch> continueCallerContext(uint64_t IRP);
  /// The WDM host first records cancellation, then asks for the one guest
  /// notification owned by this request. Ordinary WDM requests return nullopt.
  llvm::Expected<std::optional<GuestCall>> requestCancellation(uint64_t IRP);
  /// Preview the callback count before the host records an imminent cancel
  /// fact. This checks model state and token capacity without reading the host
  /// cancel flag, publishing a call, or changing references or request state.
  /// EarlierCallbacks counts other cancellation callbacks already previewed
  /// for the same boundary, so their future continuation tokens are reserved.
  llvm::Expected<bool>
  preflightRequestCancellation(uint64_t IRP,
                               uint64_t EarlierCallbacks = 0) const;
  /// Latch delivery when the scheduled or nested cancel callback is entered.
  llvm::Error beginCancelCallback(uint64_t Token);
  bool isCancelCallback(uint64_t Token) const {
    return CancelCallbacks.contains(Token);
  }
  /// Framework-owned packets must complete through their WDF request lifetime.
  bool ownsRequestIRP(uint64_t IRP) const;
  bool isPowerParkedIRP(uint64_t IRP) const;
  KernelFramework(GuestMemory &Memory, KernelExportRegistry &Exports,
                  Allocate AllocateStorage, Validate ValidateAccess,
                  Release ReleaseStorage)
      : Memory(Memory), Exports(Exports), AllocateStorage(AllocateStorage),
        ValidateAccess(ValidateAccess), ReleaseStorage(ReleaseStorage) {}

  void configure(uint64_t Driver, uint64_t RegistryPath,
                 std::string ServiceName);
  struct PnpAddDevice {
    uint64_t Callback = 0, Driver = 0, Init = 0;
  };
  bool hasPnpDriver() const;
  llvm::Expected<PnpAddDevice> beginPnpAddDevice(uint64_t PDO);
  llvm::Error finishPnpAddDevice(uint64_t PDO, uint64_t Init, uint32_t Status);
  llvm::Error removePnpDevice(uint64_t PDO);
  struct PnpCompletion {
    uint64_t IRP = 0;
    uint32_t Status = 0;
    std::optional<uint64_t> IdlePolicyDevice;
  };
  /// Run driver veto/notification callbacks before the provider receives the
  /// PnP IRP. A rejected query never changes hardware or queue power state.
  llvm::Expected<bool> beginPnpPreprocess(uint64_t PDO, uint64_t IRP,
                                          DevicePnpRequest Minor);
  /// The provider has completed successfully, but the framework may still
  /// need to run guest hardware and D0 callbacks before the IRP can unwind.
  llvm::Expected<bool> beginPnpPowerTransition(uint64_t PDO, uint64_t IRP,
                                               DevicePnpRequest Minor,
                                               uint64_t RawResources,
                                               uint64_t TranslatedResources,
                                               uint64_t ResourceListSize);
  llvm::Expected<bool> beginDevicePowerTransition(uint64_t PDO, uint64_t IRP,
                                                  DevicePowerState Previous,
                                                  DevicePowerState Target);
  llvm::Expected<bool> ownsPowerPolicy(uint64_t WdmDevice) const;
  struct PowerPolicyHost {
    enum class RequestMode { Validate, Issue };
    struct UsbIdleSettings {
      DevicePowerState DxState = DevicePowerState::D2;
      bool CanWake = false;
    };
    std::function<llvm::Expected<UsbIdleSettings>(uint64_t, uint32_t)>
        ResolveUsbIdle;
    std::function<llvm::Expected<UsbIdleKey>(uint64_t, uint64_t)> SubmitUsbIdle;
    std::function<llvm::Error(UsbIdleKey, RequestMode)> CancelUsbIdle;
    std::function<llvm::Expected<bool>(UsbIdleKey)> HasUsbIdle;
    std::function<llvm::Error(uint64_t, UsbIdleKey, uint64_t, RequestMode)>
        RequestUsbIdlePower;
    std::function<llvm::Error(UsbIdleKey, uint64_t, uint32_t)>
        AbortUsbIdlePower;
    std::function<llvm::Error(UsbIdleKey, uint64_t)> FinishUsbIdleCallback;
    std::function<uint64_t()> Now;
    std::function<llvm::Error(uint64_t, DevicePowerState, RequestMode)> Request;
    std::function<llvm::Expected<bool>(uint64_t, bool)> CanWake;
    std::function<llvm::Error(uint64_t, bool)> ArmWake;
    std::function<llvm::Error(uint64_t, bool)> FinishWake;
    std::function<llvm::Expected<std::vector<uint64_t>>(uint64_t)> Children;
    std::function<llvm::Error(uint64_t, llvm::ArrayRef<uint64_t>)>
        CompleteWakes;
    std::function<llvm::Error(llvm::ArrayRef<uint64_t>)> CancelWakes;
    std::function<llvm::Error(uint64_t, bool, uint64_t)> ManagedIdle;
    std::function<llvm::Error(uint64_t, RequestMode)> CompletePowerNotRequired;
    std::function<llvm::Error(uint64_t)> RemoveManaged;
    std::function<llvm::Expected<bool>(uint64_t, bool, bool, uint32_t)>
        ColdAllowed;
  };
  void setPowerPolicyHost(PowerPolicyHost Host) { PowerHost = std::move(Host); }
  struct PoFxHost {
    std::function<llvm::Expected<KernelPoFx::Component>(uint64_t)>
        ReadComponent;
    std::function<llvm::Error(uint64_t, const KernelFrameworkPoFxSettings &)>
        Validate;
    std::function<llvm::Expected<uint64_t>(uint64_t,
                                           const KernelFrameworkPoFxSettings &)>
        Register;
    std::function<llvm::Error(uint64_t)> Start;
    std::function<llvm::Error(uint64_t)> Quiesce;
    std::function<llvm::Expected<bool>(uint64_t)> CanUnregister;
    std::function<llvm::Error(uint64_t)> Unregister;
    std::function<llvm::Expected<bool>(uint64_t)> ComponentReady;
  };
  void setPoFxHost(PoFxHost Host) { PowerFrameworkHost = std::move(Host); }
  llvm::Error resumePoFxTransitions();
  llvm::Error powerPolicyIdle(uint64_t PDO);
  llvm::Error powerPolicyActive(uint64_t PDO);
  llvm::Error powerPolicyWake(uint64_t PDO);
  llvm::Error powerPolicyPermission(uint64_t PDO, bool NotRequired);
  llvm::Expected<std::optional<uint32_t>>
  powerPolicyDeviceCompletion(uint64_t PDO, bool Required) const;
  llvm::Expected<bool> powerPolicyDeviceReady(uint64_t PDO,
                                              bool Required) const;
  llvm::Expected<bool> allowsD3Cold(uint64_t PDO) const;
  llvm::Error canBeginUsbIdlePermission(UsbIdleKey Key, uint64_t PolicyEpoch,
                                        uint64_t CallbackToken) const;
  llvm::Error beginUsbIdlePermission(UsbIdleKey Key, uint64_t PolicyEpoch,
                                     uint64_t CallbackToken);
  llvm::Error retireUsbIdleRegistration(UsbIdleKey Key);
  llvm::Error finishUsbIdlePowerAdmissionFailure(UsbIdleKey Key,
                                                 uint64_t CallbackToken,
                                                 uint32_t Status);
  llvm::Error processPowerPolicy();
  std::optional<uint64_t> nextPowerPolicyTime() const;
  bool hasPendingPowerPolicy() const;
  llvm::Expected<std::optional<uint32_t>> powerPolicyWait(uint64_t Device);
  llvm::Expected<uint64_t> powerPolicyEpoch(uint64_t PDO) const;
  llvm::Expected<bool> systemPowerNeedsD0(uint64_t PDO) const;
  llvm::Expected<DevicePowerState> systemSleepTarget(uint64_t PDO) const;
  llvm::Expected<bool> systemSleepNeedsD0(uint64_t PDO) const;
  llvm::Error systemPowerPolicy(uint64_t PDO, bool Sleeping);
  llvm::Error beginPowerPolicyRequest(uint64_t PDO);
  llvm::Error finishPowerPolicyRequest(uint64_t PDO, uint32_t Status,
                                       bool SetPower = true);
  llvm::Error finishIdlePowerDown(uint64_t Device, uint32_t Status);

  std::optional<PnpCompletion> takePnpCompletion();
  static std::optional<unsigned>
  argumentCount(const KernelExportRegistry::Export &Export);
  llvm::Expected<uint64_t> call(const KernelExportRegistry::Export &Export,
                                llvm::ArrayRef<uint64_t> Arguments,
                                uint8_t IRQL);
  std::optional<GuestCall> takeGuestCall();
  bool hasPendingGuestCall() const { return PendingCall.has_value(); }
  /// Synchronous queue APIs wait for driver-owned requests; drain/purge also
  /// wait for requests still held by the framework queue.
  llvm::Expected<bool> queueWaitReady(uint64_t Queue,
                                      bool IncludePending) const;
  llvm::Error flushReadyNotifications();
  llvm::Error resumeInterruptDrain() { return resumePausedPnp(); }
  bool canDeliverWakeInterrupt(uint64_t PDO) const;
  /// Resume one suspended framework operation after its actual guest callback.
  llvm::Expected<std::optional<uint64_t>>
  finishGuestCall(uint64_t Token, uint64_t Result, uint8_t IRQL = 0);
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;
  bool hasLiveBinding() const;
  /// Resolve the shared device or queue callback lock. Automatic calls return
  /// zero when the object's effective synchronization scope is None.
  llvm::Expected<uint64_t> synchronizationObject(uint64_t Handle,
                                                 bool Automatic = true) const;
  uint8_t synchronizationIRQL(uint64_t Object) const;
  llvm::Expected<uint32_t> executionLevel(uint64_t Handle) const;
  llvm::Error retainSynchronizationObject(uint64_t Object);
  llvm::Error releaseSynchronizationObject(uint64_t Object);
  llvm::Error flushSynchronizationDestructions();

private:
  llvm::Expected<uint64_t> callImpl(const KernelExportRegistry::Export &Export,
                                    llvm::ArrayRef<uint64_t> Arguments,
                                    uint8_t IRQL);
  llvm::Expected<bool> deferPassiveCall(uint64_t WdmDevice = 0);
  uint64_t callbackSynchronizationObject(uint64_t Handle) const;
  llvm::Error retainQueueCallback(uint64_t Token, uint64_t Queue);
  std::set<uint64_t> PendingSynchronizationDestructions;
  uint8_t CallbackIRQL = 0;
  llvm::Error writeRequestParameters(uint64_t Address, const RequestView &View);
  llvm::Error writeRequestCompletionParams(uint64_t Address, uint32_t Status);
  GuestMemory &Memory;
  KernelExportRegistry &Exports;
  Allocate AllocateStorage;
  Validate ValidateAccess;
  Release ReleaseStorage;
  uint64_t Driver = 0, RegistryPath = 0;
  std::string ServiceName;
  struct Region {
    uint64_t Size;
    bool Writable;
    bool Opaque;
    bool Freed = false;
  };
  std::map<uint64_t, Region> Regions;
  struct Binding {
    uint64_t Info = 0, Globals = 0, Table = 0, Module = 0;
    uint64_t DriverHandle = 0, RegistryCopy = 0;
    uint64_t AddDeviceCallback = 0, UnloadCallback = 0;
    std::vector<uint8_t> RegistryBytes;
    bool Unloaded = false, Unbinding = false, Unbound = false;
  };
  std::map<uint64_t, Binding> Bindings;
  DeviceHost DevicesHost;
  InterruptHost InterruptsHost;
  LockHost LocksHost;
  struct Attributes {
    uint64_t Parent = 0, Cleanup = 0, Destroy = 0;
    uint64_t Type = 0, ContextSize = 0;
    uint32_t Execution = framework::ExecutionInherit;
    uint32_t Synchronization = framework::SynchronizationInherit;
  };
  struct FileConfig {
    bool Enabled = false;
    uint64_t Create = 0, Cleanup = 0, Close = 0;
    uint32_t AutoForward = framework::FileAutoForwardDefault;
    uint32_t Class = framework::FileObjectNotRequired;
    Attributes ObjectAttributes;
    uint64_t SynchronizationObject = 0;
    bool forwards(bool Filter) const {
      return AutoForward == framework::FileAutoForwardTrue ||
             (AutoForward == framework::FileAutoForwardDefault && Filter);
    }
  };
  struct PnpCallbacks {
#define NEVERD_FRAMEWORK_PNP_CALLBACK(Name, Index, Result) uint64_t Name = 0;
#include "KernelFrameworkPnpCallbacks.def"
#undef NEVERD_FRAMEWORK_PNP_CALLBACK
  };
  enum class DeviceInitKind { Control, Pnp };
  struct DeviceInit {
    uint64_t Binding = 0;
    DeviceInitKind Kind = DeviceInitKind::Control;
    uint64_t PDO = 0;
    std::string Name;
    std::optional<uint32_t> DeviceType;
    uint32_t IoType = framework::ControlIoBuffered;
    bool Exclusive = false;
    bool Filter = false;
    std::optional<bool> PowerPolicyOwner;
    FileConfig Files;
    uint64_t CallerContext = 0;
    PnpCallbacks Callbacks;
    KernelPowerPolicy::Callbacks PowerCallbacks;
  };
  std::map<uint64_t, DeviceInit> DeviceInits;
  struct ResourceList {
    uint64_t Handle = 0;
    uint64_t Descriptors = 0;
    uint32_t Count = 0;
  };
  enum class SelfManagedIoState {
    Uninitialized,
    Running,
    Suspended,
    Flushed,
    Cleaned
  };
  struct Device {
    uint64_t Wdm = 0, PDO = 0;
    uint64_t LocalTarget = 0;
    uint64_t DefaultQueue = 0;
    std::map<uint32_t, uint64_t> DispatchQueues;
    uint64_t CallerContext = 0;
    FileConfig Files;
    bool Filter = false;
    bool Initialized = false;
    bool PowerPolicyOwner = true;
    KernelPowerPolicy Policy;
    std::optional<KernelFrameworkPoFxSettings> PoFxSettings;
    uint64_t PoFxHandle = 0;
    bool PoFxStarted = false;
    bool HasLink = false;
    PnpCallbacks Callbacks;
    ResourceList RawResources, TranslatedResources;
    bool HardwarePrepared = false;
    bool ResourcesActive = false;
    bool InD0 = false;
    bool PowerQueuesHeld = true;
    bool PoFxComponentHeld = false;
    bool queuesHeld() const { return PowerQueuesHeld || PoFxComponentHeld; }
    uint64_t dispatchQueue(uint32_t Major) const {
      const auto Mapping = DispatchQueues.find(Major);
      return Mapping == DispatchQueues.end() ? DefaultQueue : Mapping->second;
    }
    SelfManagedIoState SelfManagedIo = SelfManagedIoState::Uninitialized;
  };
  std::map<uint64_t, Device> Devices;
  struct Interrupt {
    uint64_t Device = 0, AssociatedObject = 0;
    uint64_t ExternalLock = 0;
    uint64_t SynchronizationObject = 0;
    uint64_t ISR = 0, DPC = 0, WorkItem = 0, Enable = 0, Disable = 0;
    InterruptSelection Selection;
    std::optional<InterruptConnection> Connection;
    bool Enabled = false;
    bool ReportInactiveOnPowerDown = false;
    bool CanWake = false;
    bool ChangingState = false;
  };
  std::map<uint64_t, Interrupt> InterruptObjects;
  struct Lock {
    uint64_t Storage = 0;
    bool Wait = false;
    std::set<uint64_t> InterruptUsers;
  };
  std::map<uint64_t, Lock> LockObjects;
  llvm::Expected<std::optional<uint64_t>>
  callLock(llvm::StringRef Name, Binding &B, llvm::ArrayRef<uint64_t> Arguments,
           uint8_t IRQL);
  llvm::Error validateLockDeletion(uint64_t Handle, uint64_t Root) const;
  llvm::Error destroyFrameworkLock(uint64_t Handle);
  llvm::Expected<std::optional<uint64_t>> releaseInterruptLock(uint64_t Handle);
  enum class InterruptCallKind { Synchronize, Enable, Disable, Deferred };
  struct InterruptContinuation {
    uint64_t Object = 0;
    InterruptCallKind Kind = InterruptCallKind::Synchronize;
  };
  std::map<uint64_t, InterruptContinuation> InterruptContinuations;

  struct FileObject {
    uint64_t Device = 0, Wdm = 0;
  };
  std::map<uint64_t, FileObject> FileObjects;
  std::map<uint64_t, uint64_t> FileHandles;
  llvm::Expected<ResourceList> createResourceList(uint64_t Source,
                                                  uint64_t Size);
  llvm::Error retireResourceList(ResourceList &List);
  llvm::Error retireResourceLists(Device &D);
  std::map<uint64_t, uint64_t> PnpDeviceHandles;
  struct Queue {
    uint64_t Device = 0;
    uint64_t Default = 0, Read = 0, Write = 0, DeviceControl = 0;
    uint32_t Dispatch = framework::QueueDispatchSequential;
    uint32_t PresentedLimit = UINT32_MAX;
    bool AllowZeroLength = false;
    bool IsDefault = false;
    bool PowerManaged = false;
    bool Accepting = true;
    bool Dispatching = true;
    uint64_t StopComplete = 0;
    uint64_t StopContext = 0;
    uint64_t IoStop = 0;
    uint64_t IoResume = 0;
    uint64_t DrainComplete = 0;
    uint64_t DrainContext = 0;
    uint64_t CanceledOnQueue = 0;
    uint64_t ReadyNotify = 0;
    uint64_t ReadyContext = 0;
    bool ReadyPending = false;
    std::deque<uint64_t> Pending;
  };
  std::map<uint64_t, Queue> Queues;
  bool queuePnpHeld(const Queue &Queue) const;
  RequestHost RequestsHost;
  enum class CancelState { Unmarked, Marked, Queued, Delivered };
  struct Request {
    uint64_t IRP = 0, Queue = 0;
    uint64_t Device = 0;
    bool InCallerContext = false;
    bool Enqueued = false;
    bool Queued = false;
    bool DeliveredOnce = false;
    bool CanceledOnQueue = false;
    bool Completed = false;
    bool Completing = false;
    bool StopAcknowledged = false;
    bool PowerSuspended = false;
    uint32_t CompletionStatus = 0;
    uint64_t QueuedCallback = 0;
    std::vector<uint64_t> QueuedArguments;
    std::optional<uint32_t> QueuedCompletionStatus;
    CancelState Cancellation = CancelState::Unmarked;
    uint64_t CancelRoutine = 0;
    uint64_t File = 0;
    bool FileCreate = false;
    bool FormattedForSend = false;
    uint64_t CompletionRoutine = 0;
    uint64_t CompletionContext = 0;
    uint64_t CompletionTarget = 0;
    uint64_t PendingCompletionParams = 0;
    bool CompletionCallbackPending = false;
    bool CompletionCallbackEntered = false;
    std::optional<uint32_t> LastSendStatus;
    bool SynchronousSendPending = false;
  };
  std::map<uint64_t, Request> Requests;
  struct RequestCompletionCallback {
    uint64_t Request = 0;
    uint64_t Params = 0;
  };
  std::map<uint64_t, RequestCompletionCallback> RequestCompletionCallbacks;
  std::map<uint64_t, uint64_t> CallerRequests;
  struct RequestMemory {
    uint64_t Request = 0;
    std::optional<uint64_t> LockedMDL;
    uint64_t Buffer = 0, Length = 0;
    bool Active = true;
    std::optional<bool> Output;
  };
  std::map<uint64_t, RequestMemory> RequestMemories;
  struct Context {
    uint64_t Address = 0, Size = 0;
    uint64_t Cleanup = 0, Destroy = 0;
  };
  enum class AttributesUse { Driver, Object, AdditionalContext, Device };
  using AttributeResult = std::variant<Attributes, uint32_t>;
  enum class ObjectKind {
    Driver,
    Generic,
    Device,
    IoTarget,
    File,
    Queue,
    Request,
    Interrupt,
    SpinLock,
    WaitLock,
    Memory
  };
  struct Object {
    uint64_t Binding = 0, Parent = 0;
    uint32_t Execution = framework::ExecutionDispatch;
    uint32_t Synchronization = framework::SynchronizationNone;
    ObjectKind Kind = ObjectKind::Generic;
    bool Deleting = false, Cleaned = false;
    bool DestroyEligible = false;
    uint64_t References = 0;
    uint64_t InternalReferences = 0;
    std::map<uint64_t, Context> Contexts;
    std::vector<uint64_t> ContextOrder;
    std::vector<uint64_t> Children;
  };
  std::map<uint64_t, Object> Objects;
  enum class StepKind {
    Callback,
    PresentQueue,
    Cleaned,
    CompleteRequest,
    CompleteFileIRP,
    ForwardFileIRP,
    DeleteFileObject,
    CanceledOnQueue,
    CanceledOnQueueReturned,
    ReadyNotify,
    ReadyNotifyReturned,
    PurgeCancelRequest,
    CancelReturned,
    TryDestroy,
    Destroy,
    BeginDriverDelete,
    BeginDeviceDelete,
    FinishBindingUnbind,
    DriverUnloaded
  };
  struct Step {
    StepKind Kind;
    uint64_t Object;
    uint64_t PC = 0;
    uint64_t File = 0;
    uint32_t Status = 0;
    uint64_t SynchronizationObject = 0;
  };
  struct Continuation {
    std::vector<Step> Steps;
    size_t Index = 0;
    /// API result restored after a nested guest callback returns.
    uint64_t ReturnValue = 0;
    /// File teardown owns no queue request or power transition. Nested guest
    /// APIs retain their own continuations for queue and power notifications.
    bool AutomaticFile = false;
  };
  struct AutomaticFileForward {
    uint64_t Token = 0, File = 0;
    uint32_t Major = 0;
  };
  std::map<uint64_t, AutomaticFileForward> AutomaticFileForwards;
  uint64_t NextContinuation = 1;
  std::map<uint64_t, Continuation> Continuations;
  std::map<uint64_t, uint64_t> CancelCallbacks;
  std::map<uint64_t, uint64_t> QueueCallbacks;
  std::map<uint64_t, uint64_t> RequestDispatchQueues;
  std::map<uint64_t, uint64_t> CanceledQueueCallbacks;
  std::map<uint64_t, uint64_t> ReadyQueueCallbacks;
  PowerPolicyHost PowerHost;
  PoFxHost PowerFrameworkHost;
  llvm::Expected<std::optional<uint64_t>>
  callPoFxSettings(llvm::StringRef Name, Binding &B, llvm::ArrayRef<uint64_t> A,
                   uint8_t IRQL);
  llvm::Expected<bool> advancePoFxLifecycle(uint64_t Token);
  llvm::Expected<bool> canUnregisterPoFx(uint64_t Device) const;
  llvm::Error unregisterPoFx(uint64_t Device);
  llvm::Error holdForPoFxComponent(uint64_t Device);
  bool powerPolicyBusy(uint64_t Device) const;
  llvm::Expected<std::vector<KernelPowerPolicy::WakeChild>>
  armedWakeChildren(uint64_t Device) const;
  bool isLiveWakeChild(const KernelPowerPolicy::WakeChild &Child) const;
  llvm::Error validateWakeEnrollment(uint64_t Device) const;
  llvm::Error refreshUsbIdle(uint64_t Device);
  llvm::Error cancelUsbIdle(uint64_t Device);
  llvm::Error completeManagedUsbPowerDown(uint64_t Device,
                                          PowerPolicyHost::RequestMode Mode);
  llvm::Error restoreManagedUsbActivity(uint64_t Device);
  llvm::Error requestIdleDevicePower(uint64_t Device,
                                     PowerPolicyHost::RequestMode Mode);
  llvm::Error restartIdleTimer(uint64_t Device);
  llvm::Error beginIdlePowerDown(uint64_t Device);
  llvm::Expected<std::optional<uint64_t>>
  callPowerPolicy(llvm::StringRef Name, Binding &B, llvm::ArrayRef<uint64_t> A,
                  uint8_t IRQL);
  enum class PnpPhase {
#define NEVERD_POWER_POLICY_CALLBACK(Name, Index, Result) Name,
#include "KernelPowerPolicyCallbacks.def"
#undef NEVERD_POWER_POLICY_CALLBACK
#define NEVERD_FRAMEWORK_PNP_CALLBACK(Name, Index, Result) Name,
#include "KernelFrameworkPnpCallbacks.def"
#undef NEVERD_FRAMEWORK_PNP_CALLBACK
    IoStop,
    IoResume,
    EnableInterrupts,
    DisableInterrupts,
    DrainInterrupts,
    WakeInterrupts,
    PoFxRegister,
    PoFxStart,
    PoFxQuiesce,
    PoFxUnregister,
    DisarmWakeParents
  };
  struct PnpStep {
    PnpPhase Phase;
    uint64_t Queue = 0;
    uint64_t Request = 0;
  };
  struct PnpTransition {
    uint64_t IRP = 0, Device = 0;
    bool Entering = false;
    bool Removing = false;
    bool CallbacksComplete = false;
    bool WaitingForRequests = false;
    bool WaitingForInterrupts = false;
    bool WaitingForWakeInterrupts = false;
    bool WaitingForPoFx = false;
    uint32_t Status = 0;
    PnpStep Current{PnpPhase::PrepareHardware};
    std::deque<PnpStep> Remaining;
    std::set<uint64_t> WaitingRequests;
    bool NotificationOnly = false;
    bool IdlePolicy = false;
    bool ReleasesHardware = true;
    uint32_t PowerState = framework::PowerDeviceD3Final;
    bool SuspendAfterQueues = false;
    uint64_t CurrentInterrupt = 0;
    bool DeviceWakeEnabled = false;
    bool ChildrenArmedForWake = false;
    std::vector<KernelPowerPolicy::WakeChild> WakeChildren;
    std::deque<uint64_t> WakeParentsToDisarm;
    bool nextPrecedesRequestDrain() const {
      if (Remaining.empty())
        return false;
      const auto Phase = Remaining.front().Phase;
      return Phase == PnpPhase::IoStop || Phase == PnpPhase::SurpriseRemoval ||
             (Phase == PnpPhase::SelfManagedIoSuspend && !SuspendAfterQueues);
    }
  };
  std::map<uint64_t, PnpTransition> PnpTransitions;
  llvm::Error retireChildWake(uint64_t Device, PnpTransition &Transition);
  std::optional<PnpCompletion> CompletedPnp;
  std::optional<GuestCall> PendingCall;

  enum class PowerTransitionKind {
    Start,
    Stop,
    Remove,
    SurpriseRemoval,
    PowerUp,
    PowerDown
  };
  llvm::Expected<bool> beginPowerTransition(uint64_t PDO, uint64_t IRP,
                                            PowerTransitionKind Kind,
                                            uint32_t PowerState,
                                            uint64_t RawResources = 0,
                                            uint64_t TranslatedResources = 0,
                                            uint64_t ResourceListSize = 0);

  enum class PnpCallbackProgress { Continue, Scheduled, Waiting };
  llvm::Expected<PnpCallbackProgress> finishPnpCallback(uint64_t Token,
                                                        uint64_t Result);
  llvm::Error schedulePnpCallback(uint64_t Token);
  llvm::Error finalizePnpCallbacks(uint64_t Token);
  llvm::Error resumePausedPnp();
  bool hasPendingWakeInterrupts(uint64_t Device) const;
  llvm::Expected<bool> advancePnpInterrupts(uint64_t Token, bool Enable);
  llvm::Error finishPnpInterrupt(uint64_t Token, uint32_t Status);
  bool hasDeferredInterrupts(uint64_t Device) const;
  llvm::Error disconnectInterrupts(uint64_t Device,
                                   bool RetainInactive = false);
  llvm::Expected<uint64_t> createInterrupt(Binding &B,
                                           llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callInterrupt(llvm::StringRef Name, Binding &B,
                llvm::ArrayRef<uint64_t> Arguments, uint8_t IRQL);
  llvm::Expected<std::optional<uint64_t>>
  finishInterruptCallback(uint64_t Token, uint64_t Result);
  llvm::Error prepareInterruptCall(uint64_t Token, uint64_t Handle,
                                   uint64_t Routine,
                                   llvm::ArrayRef<uint64_t> Arguments);

  void appendPowerQueuePresentations(uint64_t Device,
                                     std::vector<Step> &Steps) const;

  llvm::Error preflightCancellationToken(uint64_t EarlierCallbacks) const;
  llvm::Expected<RequestDispatch> queueDispatch(uint64_t QueueHandle,
                                                uint64_t RequestHandle,
                                                const RequestView &View) const;
  llvm::Expected<bool> presentQueued(uint64_t QueueHandle, uint64_t Token);
  llvm::Expected<std::optional<RequestDispatch>>
  routeFileRequest(uint64_t Device, uint64_t IRP, const RequestView &View);
  llvm::Expected<uint64_t> requestFileObject(uint64_t Device,
                                             uint64_t WdmFile) const;
  llvm::Error unlinkFileObject(uint64_t File);

  llvm::Expected<uint64_t> read(uint64_t Address, unsigned Width = 8);
  llvm::Expected<std::vector<uint8_t>> readRegistryPath(uint64_t Address);
  llvm::Error writable(uint64_t Address, uint32_t Size);
  llvm::Expected<uint64_t> allocate(uint64_t Size, bool Writable, bool Opaque);
  llvm::Error retire(uint64_t Address);
  llvm::Expected<uint64_t> bind(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> unbind(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error finishUnbind(Binding &B);
  llvm::Expected<AttributeResult> attributes(uint64_t Address,
                                             AttributesUse Use);
  llvm::Expected<uint64_t> createObject(uint64_t Globals, const Attributes &A,
                                        bool IsDriver);
  llvm::Expected<uint64_t> addContext(Object &O, const Attributes &A);
  llvm::Expected<uint64_t> createDriver(Binding &B,
                                        llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callControl(llvm::StringRef Name, Binding &B,
              llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callFile(llvm::StringRef Name, Binding &B,
           llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callQueue(llvm::StringRef Name, Binding &B,
            llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callRequest(llvm::StringRef Name, Binding &B,
              llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callRequestAccessors(llvm::StringRef Name, Binding &B,
                       llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::string> readControlString(uint64_t Address);
  struct DeletionPlan {
    std::vector<Step> Steps;
    std::vector<uint64_t> Objects;
  };
  llvm::Expected<DeletionPlan> prepareDeletion(uint64_t Handle,
                                               uint64_t UnlinkedFile = 0) const;
  void commitDeletion(const DeletionPlan &Plan);
  llvm::Expected<DeletionPlan>
  prepareAutomaticFileCompletion(uint64_t IRP, uint32_t Status) const;
  llvm::Error finishAutomaticFileForward(uint64_t IRP, uint32_t Status);
  llvm::Error planDelete(uint64_t Handle, std::vector<Step> &Steps);
  llvm::Expected<std::optional<uint64_t>> advance(uint64_t Token);
  llvm::Expected<uint64_t> start(std::vector<Step> Steps);
};
} // namespace neverd::emulation
#endif
