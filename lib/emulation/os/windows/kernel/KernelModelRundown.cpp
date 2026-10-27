//===- KernelModelRundown.cpp - Run-down protection -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lock-free run-down protection as the Windows 10.0.19041.6456 kernel performs
/// it on a caller EX_RUNDOWN_REF, whose single Count word holds the run-down
/// bit (bit 0) and the outstanding acquisition count in the remaining bits,
/// scaled by two. No model state is kept; the count lives in guest memory.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

#include "neverd/emulation/DriverProfile.h"

namespace neverd::emulation {
namespace {
llvm::Error rundownError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::readRundownCount(uint64_t RunRef) {
  // The block is a single pointer-sized word; bit 0 means run-down and the
  // low two bits of a genuine pointer-aligned address are never both set. An
  // 8-byte aligned kernel address with writable storage is required, and
  // validateGuestAccess already rejects paged pool above APC_LEVEL, which is
  // the one residency rule these lock-free routines carry.
  if (!RunRef || (RunRef & 7))
    return rundownError(
        "run-down protection requires 8-byte aligned kernel storage");
  if (RunRef < profile::UserProbeLimit)
    return rundownError("run-down protection requires kernel storage");
  if (auto E = validateGuestAccess(RunRef, 8, true))
    return E;
  auto Writable = Memory.canAccess(RunRef, 8, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return rundownError("run-down protection requires writable storage");
  return Memory.readInteger(RunRef, 8);
}

llvm::Expected<uint64_t>
KernelModel::initializeRundownProtection(uint64_t RunRef) {
  // ExInitializeRundownProtection and ExReInitializeRundownProtection clear the
  // count, leaving no run-down in progress and no outstanding references.
  if (auto Count = readRundownCount(RunRef); !Count)
    return Count.takeError();
  if (auto E = Memory.writeInteger(RunRef, 0, 8))
    return E;
  return 0;
}

namespace {
// Each acquisition counts twice in the word. The counted forms double their
// ULONG in 32 bits (add edx, edx / lea r11d, [rdx+rdx]) before the 64-bit
// update, so the step wraps at 2^32 exactly as the kernel's does.
uint64_t rundownStep(uint32_t Units) {
  return static_cast<uint32_t>(Units * 2u);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::acquireRundownProtection(uint64_t RunRef,
                                                               uint32_t Units) {
  auto Count = readRundownCount(RunRef);
  if (!Count)
    return Count.takeError();
  // Once run-down has begun the object is gone; grant nothing and write
  // nothing.
  if (*Count & 1)
    return 0;
  if (auto E = Memory.writeInteger(RunRef, *Count + rundownStep(Units), 8))
    return E;
  return 1;
}

llvm::Expected<uint64_t> KernelModel::releaseRundownProtection(uint64_t RunRef,
                                                               uint32_t Units) {
  auto Count = readRundownCount(RunRef);
  if (!Count)
    return Count.takeError();
  // A release while run-down is in progress follows the kernel's wait-block
  // path, which on a completed run-down decrements through a null pointer; it
  // also implies a blocked waiter this cooperative processor cannot run.
  if (*Count & 1)
    return rundownError("cannot release run-down protection once run-down has "
                        "started on this cooperative processor");
  const uint64_t Step = rundownStep(Units);
  if (*Count < Step)
    return rundownError("run-down protection release without a matching "
                        "acquisition");
  if (auto E = Memory.writeInteger(RunRef, *Count - Step, 8))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::waitForRundownProtectionRelease(uint64_t RunRef) {
  auto Count = readRundownCount(RunRef);
  if (!Count)
    return Count.takeError();
  // No outstanding references: mark run-down and return at once.
  if (*Count == 0) {
    if (auto E = Memory.writeInteger(RunRef, 1, 8))
      return E;
    return 0;
  }
  // Already run down: a repeat wait has no effect and is not an error.
  if (*Count & 1)
    return 0;
  // Outstanding references would block on an event until each is released;
  // another thread cannot run to release them here.
  return rundownError("cannot wait for outstanding run-down references on this "
                      "cooperative processor");
}

llvm::Expected<uint64_t> KernelModel::rundownCompleted(uint64_t RunRef) {
  // ExRundownCompleted marks the object run down unconditionally.
  if (auto Count = readRundownCount(RunRef); !Count)
    return Count.takeError();
  if (auto E = Memory.writeInteger(RunRef, 1, 8))
    return E;
  return 0;
}

} // namespace neverd::emulation
