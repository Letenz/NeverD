//===- AndroidObservation.cpp - Read-only execution evidence -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Capture explicit scalar state and memory without patching guest code.
#include "AndroidObservation.h"

#include "AndroidInternal.h"

#include <set>

namespace neverd::emulation::android_model {
llvm::Error
validateExecutionObservation(const NativeExecutionObservationOptions &Options) {
  if (Options.PCs.empty() || Options.PCs.size() > 64 || !Options.Limit)
    return failure(diagnostic::ObservationOptions);
  std::set<uint64_t> Unique;
  for (uint64_t PC : Options.PCs)
    if (PC % 4 || !Unique.insert(PC).second)
      return failure(diagnostic::ObservationOptions);
  for (const auto &Read : Options.ReadMemory)
    if (!Read.Size || Read.Address > UINT64_MAX - Read.Size)
      return failure(diagnostic::SnapshotLimits);
  return llvm::Error::success();
}
llvm::Expected<uint64_t>
executionObservationBytes(const NativeExecutionObservationOptions &Options,
                          uint64_t Available) {
  if (auto E = validateExecutionObservation(Options))
    return std::move(E);
  // Scalar record plus each explicit extent and its bytes. Reserve all
  // retained records before execution, using division to avoid overflow.
  uint64_t PerRecord = 36 * sizeof(uint64_t);
  if (PerRecord > Available)
    return failure(diagnostic::SnapshotLimits);
  for (const auto &Read : Options.ReadMemory) {
    if (16 > Available - PerRecord)
      return failure(diagnostic::SnapshotLimits);
    PerRecord += 16;
    if (Read.Size > Available - PerRecord)
      return failure(diagnostic::SnapshotLimits);
    PerRecord += Read.Size;
  }
  if (Options.Limit > Available / PerRecord)
    return failure(diagnostic::SnapshotLimits);
  return Options.Limit * PerRecord;
}
llvm::Expected<NativeExecutionObservation> captureExecutionObservation(
    ExecutionBackend &CPU, const NativeExecutionObservationOptions &Options,
    uint64_t PC, uint64_t ThreadID, uint64_t Instructions) {
  NativeExecutionObservation Out;
  Out.PC = PC;
  Out.ThreadID = ThreadID;
  Out.Instructions = Instructions;
  for (unsigned I = 0; I < Out.Registers.size(); ++I) {
    auto R =
        CPU.readRegister(CPURegister(unsigned(CPURegister::AArch64X0) + I));
    if (!R)
      return R.takeError();
    Out.Registers[I] = (*R)[0];
  }
  auto SP = CPU.readRegister(CPURegister::AArch64SP);
  if (!SP)
    return SP.takeError();
  Out.SP = (*SP)[0];
  auto NZCV = CPU.readRegister(CPURegister::AArch64NZCV);
  if (!NZCV)
    return NZCV.takeError();
  Out.NZCV = (*NZCV)[0];
  for (const auto &Read : Options.ReadMemory) {
    auto Access = CPU.canAccess(Read.Address, Read.Size,
                                emulation::Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return failure(diagnostic::ObservationMemory);
    NativeMemorySnapshot Snapshot{Read.Address,
                                  std::vector<uint8_t>(Read.Size)};
    if (auto E = CPU.read(Read.Address, Snapshot.Bytes))
      return std::move(E);
    Out.Memory.push_back(std::move(Snapshot));
  }
  return Out;
}
} // namespace neverd::emulation::android_model
