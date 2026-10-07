//===- MemoryProjection.h - Per-CPU projection over shared RAM ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_MEMORYPROJECTION_H
#define NEVERD_EMULATION_CORE_MEMORYPROJECTION_H
#include "MachineRunControl.h"
#include "MemoryStorage.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <array>
#include <optional>

namespace neverd::emulation {
struct MemoryRegistration {
  uint64_t Physical;
  uint8_t *Backing;
  uint64_t Size;
};
/// Opaque execution can write RAM without publishing an instruction footprint.
/// Declared execution invalidates reservations at each committed write instead.
enum class RAMWriteTracking { Opaque, Declared };
/// Owns only private monitor/page-table storage. AddressSpace is the mapping
/// authority; physical allocations are shared and retained by every projection.
class MemoryProjection {
public:
  class InstructionLease {
  public:
    InstructionLease(InstructionLease &&Other) noexcept;
    ~InstructionLease();
    InstructionLease(const InstructionLease &) = delete;
    InstructionLease &operator=(const InstructionLease &) = delete;

  private:
    friend class MemoryProjection;
    InstructionLease(MemoryProjection *Memory,
                     std::unique_lock<std::recursive_mutex> Lock);
    MemoryProjection *Memory;
    std::unique_lock<std::recursive_mutex> Lock;
  };
  using Page = AddressSpace::Impl::Page;
  using Device = AddressSpace::Impl::Device;
  static llvm::Expected<std::unique_ptr<MemoryProjection>>
  create(uint64_t Limit);
  static llvm::Expected<std::unique_ptr<MemoryProjection>>
  create(std::shared_ptr<AddressSpace> Space);
  ~MemoryProjection();
  std::shared_ptr<AddressSpace> addressSpace() const { return Space; }
  llvm::Expected<std::unique_lock<std::recursive_mutex>> lock() const;
  /// Borrow the current thread's active physical execution lease. A stopped
  /// owner or a different host thread cannot stage private instruction effects.
  llvm::Expected<std::unique_lock<std::recursive_mutex>> executionLock() const;
  llvm::Error mutableMemory() const;
  llvm::Error
  validateMappings(llvm::function_ref<bool(uint64_t, uint64_t)> Valid,
                   bool AllowDevices = false) const;
  llvm::Error bind(std::shared_ptr<AddressSpace> Next,
                   llvm::function_ref<bool(uint64_t, uint64_t)> Valid,
                   bool AllowDevices = false);
  llvm::Error beginRun(RAMWriteTracking Tracking = RAMWriteTracking::Opaque);
  void endRun();
  /// Configure before constructing the transport. Independent WHP processors
  /// use private registration bytes; all observations still use shared RAM.
  llvm::Error enableParallel(bool PrivateTransportRAM = false);
  bool parallelEnabled() const { return ParallelEnabled; }
  llvm::Error beginParallelRun(MachineRunControl Control);
  llvm::Expected<InstructionLease> beginInstruction();
  /// Called before a write transaction borrows an additional recursive lock.
  llvm::Error prepareWrite();
  /// The caller has proven a zero-write footprint and released extra borrows.
  /// All native state capture and interrupt acknowledgement finish inside F.
  llvm::Error executeReadOnly(llvm::function_ref<llvm::Error()> F);
  llvm::Error prepareTransportRead(uint64_t Address, uint64_t Size);
  void captureTransportWrite(uint64_t Physical, uint64_t Size);
  /// Temporarily project one already admitted device operand into private
  /// processor scratch. This never changes AddressSpace or invokes a device.
  llvm::Error stageDeviceOperand(uint64_t Address, uint64_t Physical,
                                 llvm::ArrayRef<uint8_t> Bytes);
  void retireDeviceOperand();
  std::optional<std::pair<uint64_t, uint64_t>> deviceOperand() const {
    return DeviceOperand;
  }
  /// Establish/test a reservation under a declared physical execution lease.
  /// The ISA selects a power-of-two granule within one physical page and owns
  /// instruction permissions, alignment, local monitor lifetime and status.
  /// Matching requires the same operand width within the reserved physical
  /// granule; the original byte offset is not a second address-identity test.
  llvm::Expected<std::shared_ptr<RAMReservation>>
  reserveRAM(uint64_t Address, uint64_t Size, uint64_t Granule);
  llvm::Expected<bool>
  reservationMatches(const std::shared_ptr<RAMReservation> &Reservation,
                     uint64_t Address, uint64_t Size) const;
  /// Publish an already validated physical write under the execution lease.
  /// Do not call for temporary processor writes or transaction rollback.
  void recordRAMWrite(uint64_t Physical, uint64_t Size);
  bool setWriteWatches(const std::vector<MemoryWriteWatch> &Watches);
  /// Tests physical bytes, so writes through an unobserved alias still match.
  bool writeWatched(uint64_t Physical, uint64_t Size);
  bool takeWatchedWrite() { return std::exchange(WatchedWrite, false); }
  llvm::Error map(uint64_t A, uint64_t N, unsigned P) {
    return Space->map(A, N, P);
  }
  llvm::Error aliases(llvm::ArrayRef<GuestAliasRange> Remove,
                      llvm::ArrayRef<GuestAliasMapping> Add) {
    return Space->replaceAliases(Remove, Add);
  }
  llvm::Error protect(uint64_t A, uint64_t N, unsigned P) {
    return Space->protect(A, N, P);
  }
  std::optional<MemoryAccessFailure> firstAccessFailure(uint64_t A, uint64_t N,
                                                        unsigned P) const;
  std::optional<BackendFaultKind> check(uint64_t A, uint64_t N,
                                        unsigned P) const;
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B,
                   unsigned P = Read) const {
    return Space->readWithPermissions(A, B, P);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B, unsigned P = Write) {
    return Space->writeWithPermissions(A, B, P);
  }
  const std::map<uint64_t, Page> &mappings() const {
    return Space->State->Pages;
  }
  uint64_t mappingGeneration() const { return Space->mappingGeneration(); }
  bool needsProjection(GuestArchitecture Architecture, bool UserMode = false,
                       uint64_t Variant = 0) const {
    return ProjectedSpace.lock() != Space ||
           Generation != mappingGeneration() || ProjectedUserMode != UserMode ||
           ProjectedVariant != Variant || ProjectedArchitecture != Architecture;
  }
  const std::map<uint64_t, std::shared_ptr<Device>> &devices() const {
    return Space->State->Devices;
  }
  /// Cache identity and the current opaque root belong to these private bytes,
  /// not to one caller. The ISA builder owns root selection and page encoding.
  /// Invalidate before rewriting bytes, retaining root history and RAM pins
  /// until a complete replacement is published.
  void invalidateProjection() { ProjectedArchitecture.reset(); }
  void commitProjection(GuestArchitecture Architecture, bool UserMode = false,
                        uint64_t Variant = 0, uint64_t Root = 0);
  uint64_t projectionRoot(GuestArchitecture Architecture) const {
    auto I = ProjectedRoots.find(Architecture);
    return I == ProjectedRoots.end() ? 0 : I->second;
  }
  uint8_t *data() const { return static_cast<uint8_t *>(Projection.base()); }
  /// Relocate only the transport's physical window. Guest mappings, RAM
  /// transaction offsets and private-storage offsets retain their identities.
  llvm::Error relocateTransport(uint64_t Base);
  uint64_t transportPhysical(uint64_t Offset) const {
    return TransportBase + Offset;
  }
  uint64_t transportOffset(uint64_t Physical) const {
    return Physical - TransportBase;
  }
  std::array<MemoryRegistration, 2> registrations() const;
  uint8_t *physicalPointer(uint64_t GPA) const;

private:
  MemoryProjection(std::shared_ptr<AddressSpace> Space,
                   llvm::sys::MemoryBlock Projection);
  std::shared_ptr<AddressSpace> Space;
  llvm::sys::MemoryBlock Projection;
  uint64_t Generation = 0;
  bool ProjectedUserMode = false;
  uint64_t ProjectedVariant = 0;
  std::map<GuestArchitecture, uint64_t> ProjectedRoots;
  std::optional<GuestArchitecture> ProjectedArchitecture;
  std::weak_ptr<AddressSpace> ProjectedSpace;
  // Keep old allocations pinned until the transport retires the old mapping.
  std::map<uint64_t, Page> ProjectedPages;
  RAMWriteTracking WriteTracking = RAMWriteTracking::Opaque;
  bool ParallelEnabled = false, ParallelRunning = false;
  bool InstructionActive = false;
  std::thread::id RunThread;
  MachineRunControl ParallelControl{};
  llvm::sys::MemoryBlock TransportRAM;
  uint64_t TransportBase = 0;
  std::optional<std::pair<uint64_t, uint64_t>> DeviceOperand;
  std::vector<MemoryWriteWatch> WriteWatches, PhysicalWriteWatches;
  std::weak_ptr<AddressSpace> WriteWatchSpace;
  uint64_t WriteWatchGeneration = 0;
  bool WatchedWrite = false;
  void refreshWriteWatches();
  void finishInstruction();
};
} // namespace neverd::emulation
#endif
