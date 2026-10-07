//===- ParallelExecution.cpp - Shared checked CPU admission --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"
#include "MemoryProjection.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

namespace neverd::emulation {
namespace {
using Clock = std::chrono::steady_clock;
auto nextPoll(MachineRunControl Control) {
  return std::min(
      Control.Deadline,
      Clock::now() +
          std::chrono::microseconds(execution_limits::CancelRetryMicroseconds));
}
llvm::Expected<std::unique_lock<std::recursive_mutex>>
acquire(std::recursive_mutex &Mutex, MachineRunControl Control) {
  std::unique_lock Lock(Mutex, std::defer_lock);
  for (;;) {
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::ParallelWait, Control);
    if (Lock.try_lock())
      return Lock;
    std::this_thread::sleep_until(nextPoll(Control));
  }
}
} // namespace

MemoryProjection::InstructionLease::InstructionLease(
    MemoryProjection *Memory, std::unique_lock<std::recursive_mutex> Lock)
    : Memory(Memory), Lock(std::move(Lock)) {}
MemoryProjection::InstructionLease::InstructionLease(
    InstructionLease &&Other) noexcept
    : Memory(std::exchange(Other.Memory, nullptr)),
      Lock(std::move(Other.Lock)) {}
MemoryProjection::InstructionLease::~InstructionLease() {
  if (Memory)
    Memory->finishInstruction();
}

llvm::Error MemoryProjection::enableParallel(bool PrivateTransportRAM) {
  auto Lease = lock();
  if (!Lease)
    return Lease.takeError();
  if (auto E = mutableMemory())
    return E;
  if (ParallelEnabled)
    return diagnostic::error(diagnostic::Running);
  if (PrivateTransportRAM) {
    std::error_code EC;
    auto Backing = llvm::sys::Memory::allocateMappedMemory(
        Space->State->Memory->State->Backing.allocatedSize(), nullptr,
        llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
    if (EC)
      return llvm::errorCodeToError(EC);
    TransportRAM = Backing;
  }
  ParallelEnabled = true;
  return llvm::Error::success();
}

llvm::Error MemoryProjection::beginParallelRun(MachineRunControl Control) {
  auto &RAM = *Space->State->Memory->State;
  auto Lease = acquire(RAM.Mutex, Control);
  if (!Lease)
    return Lease.takeError();
  const auto Thread = std::this_thread::get_id();
  if (!ParallelEnabled || ParallelRunning ||
      (RAM.Running && RAM.ParallelRuns.empty()) ||
      RAM.ParallelRuns.count(Thread))
    return diagnostic::error(diagnostic::Running);
  RAM.ParallelRuns.emplace(Thread, this);
  WatchedWrite = false;
  RAM.Running = ParallelRunning = true;
  RunThread = Thread;
  ParallelControl = Control;
  WriteTracking = RAMWriteTracking::Declared;
  return llvm::Error::success();
}

llvm::Expected<MemoryProjection::InstructionLease>
MemoryProjection::beginInstruction() {
  if (!ParallelRunning)
    return InstructionLease(nullptr, {});
  if (RunThread != std::this_thread::get_id() || InstructionActive)
    return diagnostic::error(diagnostic::RAMTransactionLease);
  auto &RAM = *Space->State->Memory->State;
  auto Lease = acquire(RAM.Mutex, ParallelControl);
  if (!Lease)
    return Lease.takeError();
  while (RAM.Writer) {
    if (ParallelControl.interrupted())
      return diagnostic::interrupted(diagnostic::ParallelWait, ParallelControl);
    RAM.Changed.wait_until(*Lease, nextPoll(ParallelControl));
  }
  if (ParallelControl.interrupted())
    return diagnostic::interrupted(diagnostic::ParallelWait, ParallelControl);
  InstructionActive = true;
  return InstructionLease(this, std::move(*Lease));
}

void MemoryProjection::finishInstruction() {
  auto &RAM = *Space->State->Memory->State;
  assert(ParallelRunning && InstructionActive &&
         RunThread == std::this_thread::get_id());
  if (RAM.Writer == this)
    RAM.Writer = nullptr;
  InstructionActive = false;
  RAM.Changed.notify_all();
}

llvm::Error MemoryProjection::prepareWrite() {
  if (!ParallelRunning)
    return llvm::Error::success();
  if (!InstructionActive || RunThread != std::this_thread::get_id())
    return diagnostic::error(diagnostic::RAMTransactionLease);
  auto &RAM = *Space->State->Memory->State;
  assert(!RAM.Writer || RAM.Writer == this);
  RAM.Writer = this;
  // Exactly the instruction guard owns the mutex here. Transactions borrow
  // another recursive level only after this wait has completed.
  while (RAM.Readers) {
    if (ParallelControl.interrupted())
      return diagnostic::interrupted(diagnostic::ParallelWait, ParallelControl);
    RAM.Changed.wait_until(RAM.Mutex, nextPoll(ParallelControl));
  }
  if (ParallelControl.interrupted())
    return diagnostic::interrupted(diagnostic::ParallelWait, ParallelControl);
  return llvm::Error::success();
}

llvm::Error
MemoryProjection::executeReadOnly(llvm::function_ref<llvm::Error()> F) {
  if (!ParallelRunning)
    return F();
  assert(InstructionActive && RunThread == std::this_thread::get_id());
  auto &RAM = *Space->State->Memory->State;
  assert(!RAM.Writer);
  ++RAM.Readers;
  RAM.Mutex.unlock();
  auto Retire = llvm::scope_exit([&] {
    // Writers release the mutex while draining readers. Reacquisition must
    // complete even after cancellation or a throwing native adapter.
    RAM.Mutex.lock();
    --RAM.Readers;
    RAM.Changed.notify_all();
  });
  return F();
}

llvm::Error MemoryProjection::prepareTransportRead(uint64_t Address,
                                                   uint64_t Size) {
  if (!TransportRAM.base())
    return llvm::Error::success();
  if (Space->State->check(Address, Size, 0) ||
      Space->State->overlapsDevice(Address, Size))
    return diagnostic::error(diagnostic::RAMTransactionRange);
  while (Size) {
    const uint64_t Offset = Address % memory::PageSize;
    const auto &Page = mappings().at(Address - Offset);
    const uint64_t Count = std::min(Size, memory::PageSize - Offset);
    std::memcpy(static_cast<uint8_t *>(TransportRAM.base()) + Page.Physical -
                    memory::ProjectionReserve + Offset,
                physicalPointer(Page.Physical + Offset), Count);
    Address += Count;
    Size -= Count;
  }
  return llvm::Error::success();
}

void MemoryProjection::captureTransportWrite(uint64_t Physical, uint64_t Size) {
  if (!TransportRAM.base())
    return;
  assert(Physical >= memory::ProjectionReserve);
  assert(Physical - memory::ProjectionReserve + Size <=
         TransportRAM.allocatedSize());
  std::memcpy(physicalPointer(Physical),
              static_cast<const uint8_t *>(TransportRAM.base()) + Physical -
                  memory::ProjectionReserve,
              Size);
}
} // namespace neverd::emulation
