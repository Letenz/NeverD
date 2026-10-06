//===- LinuxResidency.cpp - Mincore validation and mapping boundaries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxMemory.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxMemory::residency(const ProcessServiceEvent &Event,
                       ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  const uint64_t Address = Event.Arguments[0], Length = Event.Arguments[1];
  const uint64_t Vector = Event.Arguments[2];
  if (Address % PageSize)
    return Reply(uint64_t(0) - InvalidArgument);
  if (Address > UserLimit || Length > UserLimit - Address)
    return Reply(uint64_t(0) - NoMemory);
  const uint64_t Pages = Length / PageSize + (Length % PageSize != 0);
  // access_ok checks the numerical range, not the vector's mapping. A hole
  // at the first queried page is reported before copy_to_user can run.
  if (Vector > UserLimit || Pages > UserLimit - Vector)
    return Reply(uint64_t(0) - BadAddress);
  if (!Pages)
    return Reply(0);
  auto Mappings = Space.mappings();
  if (!Mappings)
    return Mappings.takeError();
  for (const auto &Mapping : *Mappings) {
    if (Address < Mapping.Address || Address - Mapping.Address >= Mapping.Size)
      continue;
    // A mapping, including PROT_NONE, does not prove Linux residency. Do not
    // skip a mapped prefix to report a later hole: that can discard writes
    // already copied to the caller's residency vector.
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = MemoryResidency;
    return Reply();
  }
  return Reply(uint64_t(0) - NoMemory);
}
} // namespace neverd::emulation::linux_model
