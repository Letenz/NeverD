//===- MemoryProjection.cpp - Private CPU projection storage -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "MemoryProjection.h"

#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"

#include <cassert>

namespace neverd::emulation {
MemoryProjection::MemoryProjection(std::shared_ptr<AddressSpace> Space,
                                   llvm::sys::MemoryBlock Projection)
    : Space(std::move(Space)), Projection(Projection) {}
MemoryProjection::~MemoryProjection() {
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
llvm::Error MemoryProjection::beginRun() {
  auto &RAM = *Space->State->Memory->State;
  if (!RAM.Mutex.try_lock())
    return diagnostic::error(diagnostic::Running);
  if (RAM.Running) {
    RAM.Mutex.unlock();
    return diagnostic::error(diagnostic::Running);
  }
  RAM.Running = true;
  return llvm::Error::success();
}
void MemoryProjection::endRun() {
  auto &RAM = *Space->State->Memory->State;
  RAM.Running = false;
  RAM.Mutex.unlock();
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
void MemoryProjection::commitProjection(bool UserMode, uint64_t Variant) {
  ProjectedUserMode = UserMode;
  ProjectedVariant = Variant;
  ProjectedPages = mappings();
  Generation = mappingGeneration();
  ProjectedSpace = Space;
}
std::array<MemoryRegistration, 2> MemoryProjection::registrations() const {
  auto &Backing = Space->State->Memory->State->Backing;
  return {{{0, data(), memory::ProjectionReserve},
           {memory::ProjectionReserve, static_cast<uint8_t *>(Backing.base()),
            Backing.allocatedSize()}}};
}
uint8_t *MemoryProjection::physicalPointer(uint64_t GPA) const {
  if (GPA < memory::ProjectionReserve)
    return data() + GPA;
  auto &Backing = Space->State->Memory->State->Backing;
  const uint64_t Offset = GPA - memory::ProjectionReserve;
  assert(Offset < Backing.allocatedSize());
  return static_cast<uint8_t *>(Backing.base()) + Offset;
}
} // namespace neverd::emulation
