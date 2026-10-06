//===- MemoryProjection.cpp - Private CPU projection storage -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "MemoryProjection.h"

#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace neverd::emulation {
MemoryProjection::MemoryProjection(std::shared_ptr<AddressSpace> Space,
                                   llvm::sys::MemoryBlock Projection)
    : Space(std::move(Space)), Projection(Projection) {}
MemoryProjection::~MemoryProjection() {
  if (TransportRAM.base())
    (void)llvm::sys::Memory::releaseMappedMemory(TransportRAM);
  (void)llvm::sys::Memory::releaseMappedMemory(Projection);
}
llvm::Expected<std::unique_ptr<MemoryProjection>>
MemoryProjection::create(uint64_t Limit) {
  auto RAM = PhysicalMemory::create(Limit);
  if (!RAM)
    return RAM.takeError();
  auto Space = AddressSpace::create(std::move(*RAM), Limit);
  if (!Space)
    return Space.takeError();
  return create(std::move(*Space));
}
llvm::Expected<std::unique_ptr<MemoryProjection>>
MemoryProjection::create(std::shared_ptr<AddressSpace> Space) {
  if (!Space)
    return diagnostic::error(diagnostic::AddressSpace);
  std::error_code EC;
  auto Block = llvm::sys::Memory::allocateMappedMemory(
      memory::ProjectionReserve, nullptr,
      llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
  if (EC)
    return llvm::errorCodeToError(EC);
  return std::unique_ptr<MemoryProjection>(
      new MemoryProjection(std::move(Space), Block));
}
llvm::Expected<std::unique_lock<std::recursive_mutex>>
MemoryProjection::lock() const {
  std::unique_lock Lock(Space->State->Memory->State->Mutex, std::try_to_lock);
  if (!Lock.owns_lock())
    return diagnostic::error(diagnostic::Running);
  return Lock;
}
llvm::Expected<std::unique_lock<std::recursive_mutex>>
MemoryProjection::executionLock() const {
  auto Lease = lock();
  if (!Lease)
    return Lease.takeError();
  const auto &RAM = *Space->State->Memory->State;
  if (!RAM.Running || (!RAM.ParallelRuns.empty() && !ParallelRunning) ||
      (ParallelRunning &&
       (RunThread != std::this_thread::get_id() || !InstructionActive)))
    return diagnostic::error(diagnostic::RAMTransactionLease);
  return std::move(*Lease);
}
llvm::Error MemoryProjection::mutableMemory() const {
  auto &RAM = *Space->State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  return llvm::Error::success();
}
llvm::Error MemoryProjection::validateMappings(
    llvm::function_ref<bool(uint64_t, uint64_t)> Valid,
    bool AllowDevices) const {
  std::unique_lock Lock(Space->State->Memory->State->Mutex, std::try_to_lock);
  if (!Lock.owns_lock())
    return diagnostic::error(diagnostic::Running);
  for (const auto &[Address, Page] : mappings()) {
    if (!Valid(Address, memory::PageSize))
      return diagnostic::error(diagnostic::InvalidMapping);
    if (Page.IO && !AllowDevices)
      return diagnostic::error(diagnostic::DeviceMapping);
  }
  return llvm::Error::success();
}
llvm::Error
MemoryProjection::bind(std::shared_ptr<AddressSpace> Next,
                       llvm::function_ref<bool(uint64_t, uint64_t)> Valid,
                       bool AllowDevices) {
  auto &RAM = *Space->State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  if (!Next || Next->physicalMemory() != Space->physicalMemory())
    return diagnostic::error(diagnostic::MemoryOwner);
  for (const auto &[Address, Page] : Next->State->Pages) {
    if (!Valid(Address, memory::PageSize))
      return diagnostic::error(diagnostic::InvalidMapping);
    if (Page.IO && !AllowDevices)
      return diagnostic::error(diagnostic::DeviceMapping);
  }
  Space = std::move(Next);
  return llvm::Error::success();
}
llvm::Error MemoryProjection::beginRun(RAMWriteTracking Tracking) {
  auto &RAM = *Space->State->Memory->State;
  if (!RAM.Mutex.try_lock())
    return diagnostic::error(diagnostic::Running);
  if (RAM.Running) {
    RAM.Mutex.unlock();
    return diagnostic::error(diagnostic::Running);
  }
  RAM.Running = true;
  WriteTracking = Tracking;
  if (Tracking == RAMWriteTracking::Opaque)
    RAM.invalidateReservations();
  return llvm::Error::success();
}
void MemoryProjection::endRun() {
  auto &RAM = *Space->State->Memory->State;
  if (ParallelRunning) {
    std::lock_guard Lock(RAM.Mutex);
    assert(RunThread == std::this_thread::get_id() && !InstructionActive);
    RAM.ParallelRuns.erase(RunThread);
    RAM.Running = !RAM.ParallelRuns.empty();
    ParallelRunning = false;
    RunThread = {};
    RAM.Changed.notify_all();
    return;
  }
  if (WriteTracking == RAMWriteTracking::Opaque)
    RAM.invalidateReservations();
  RAM.Running = false;
  RAM.Mutex.unlock();
}
llvm::Expected<std::shared_ptr<RAMReservation>>
MemoryProjection::reserveRAM(uint64_t Address, uint64_t Size,
                             uint64_t Granule) {
  auto Lease = executionLock();
  if (!Lease)
    return Lease.takeError();
  if (WriteTracking != RAMWriteTracking::Declared)
    return diagnostic::error(diagnostic::RAMReservationTracking);
  if (!llvm::isPowerOf2_64(Granule) || Granule > memory::PageSize || !Size ||
      Size > Granule || Address % Granule > Granule - Size)
    return diagnostic::error(diagnostic::RAMReservationRange);
  auto Bytes = Space->pinBacking(Address, Size);
  if (!Bytes)
    return Bytes.takeError();
  const uint64_t Offset = Address % memory::PageSize;
  const uint64_t Physical = mappings().at(Address - Offset).Physical + Offset;
  auto R = std::make_shared<RAMReservation>(
      RAMReservation{std::move(*Bytes), Physical & ~(Granule - 1), Granule});
  auto &Reservations = Space->State->Memory->State->Reservations;
  std::erase_if(Reservations, [](const auto &R) { return R.expired(); });
  Reservations.push_back(R);
  return R;
}
llvm::Expected<bool>
MemoryProjection::reservationMatches(const std::shared_ptr<RAMReservation> &R,
                                     uint64_t Address, uint64_t Size) const {
  auto Lease = executionLock();
  if (!Lease)
    return Lease.takeError();
  if (WriteTracking != RAMWriteTracking::Declared)
    return diagnostic::error(diagnostic::RAMReservationTracking);
  if (!R || R->Bytes.physicalMemory() != Space->physicalMemory() || !R->Valid ||
      Size != R->Bytes.size())
    return false;
  const uint64_t Offset = Address % memory::PageSize;
  const auto P = mappings().find(Address - Offset);
  if (P == mappings().end() || P->second.IO || Size > memory::PageSize - Offset)
    return false;
  const uint64_t Physical = P->second.Physical + Offset;
  return Physical >= R->Granule &&
         Physical - R->Granule <= R->GranuleSize - Size;
}
void MemoryProjection::recordRAMWrite(uint64_t Physical, uint64_t Size) {
  auto &RAM = *Space->State->Memory->State;
  assert(RAM.Running && diagnostic::RAMTransactionLease);
  RAM.invalidateReservations(Physical, Size);
}
std::optional<MemoryAccessFailure>
MemoryProjection::firstAccessFailure(uint64_t A, uint64_t N, unsigned P) const {
  std::lock_guard Lock(Space->State->Memory->State->Mutex);
  return Space->State->firstAccessFailure(A, N, P);
}
std::optional<BackendFaultKind> MemoryProjection::check(uint64_t A, uint64_t N,
                                                        unsigned P) const {
  std::lock_guard Lock(Space->State->Memory->State->Mutex);
  return Space->State->check(A, N, P);
}
void MemoryProjection::commitProjection(GuestArchitecture Architecture,
                                        bool UserMode, uint64_t Variant,
                                        uint64_t Root) {
  ProjectedArchitecture = Architecture;
  ProjectedRoots[Architecture] = Root;
  ProjectedUserMode = UserMode;
  ProjectedVariant = Variant;
  ProjectedPages = mappings();
  Generation = mappingGeneration();
  ProjectedSpace = Space;
}
std::array<MemoryRegistration, 2> MemoryProjection::registrations() const {
  const auto &Backing =
      TransportRAM.base() ? TransportRAM : Space->State->Memory->State->Backing;
  return {{{transportPhysical(0), data(), memory::ProjectionReserve},
           {transportPhysical(memory::ProjectionReserve),
            static_cast<uint8_t *>(Backing.base()), Backing.allocatedSize()}}};
}
llvm::Error MemoryProjection::relocateTransport(uint64_t Base) {
  auto Lease = lock();
  if (!Lease)
    return Lease.takeError();
  if (auto E = mutableMemory())
    return E;
  const auto Size = memory::ProjectionReserve +
                    Space->State->Memory->State->Backing.allocatedSize();
  if (Base % memory::PageSize || Base > memory::MaxTransportAddress ||
      Size - 1 > memory::MaxTransportAddress - Base)
    return diagnostic::error(diagnostic::InvalidMapping);
  TransportBase = Base;
  invalidateProjection();
  return llvm::Error::success();
}
uint8_t *MemoryProjection::physicalPointer(uint64_t GPA) const {
  if (GPA < memory::ProjectionReserve)
    return data() + GPA;
  auto &Backing = Space->State->Memory->State->Backing;
  const uint64_t Offset = GPA - memory::ProjectionReserve;
  assert(Offset < Backing.allocatedSize());
  return static_cast<uint8_t *>(Backing.base()) + Offset;
}
llvm::Error
MemoryProjection::stageDeviceOperand(uint64_t Address, uint64_t Physical,
                                     llvm::ArrayRef<uint8_t> Bytes) {
  auto Lease = executionLock();
  if (!Lease)
    return Lease.takeError();
  const uint64_t Offset = Address % memory::PageSize;
  const auto P = mappings().find(Address - Offset);
  if (DeviceOperand || Bytes.empty() ||
      Bytes.size() > execution_limits::MMIOAtomicBytes ||
      Bytes.size() > memory::PageSize - Offset || Physical % memory::PageSize ||
      Physical > memory::ProjectionReserve - memory::PageSize ||
      P == mappings().end() || !P->second.IO)
    return diagnostic::error(diagnostic::DeviceOperand);
  std::memset(data() + Physical, 0, memory::PageSize);
  std::memcpy(data() + Physical + Offset, Bytes.data(), Bytes.size());
  DeviceOperand = std::pair(Address - Offset, Physical);
  invalidateProjection();
  return llvm::Error::success();
}
void MemoryProjection::retireDeviceOperand() {
  DeviceOperand.reset();
  invalidateProjection();
}
} // namespace neverd::emulation
