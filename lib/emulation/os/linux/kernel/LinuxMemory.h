//===- LinuxMemory.h - Linux anonymous memory services ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXMEMORY_H
#define NEVERD_EMULATION_OS_LINUX_LINUXMEMORY_H

#include "LinuxKernel.h"

#include "neverd/emulation/AddressSpace.h"

namespace neverd::emulation::linux_model {
/// Own Linux placement and program-break policy for one stopped process.
/// AddressSpace remains the only mapping/permission authority. These services
/// require exclusive OS ownership of mutations across the whole operation.
class LinuxMemory {
public:
  LinuxMemory(AddressSpace &Space, const MemoryLayout &Layout,
              uint64_t InitialBreak, const ProcessOptions &Options);
  llvm::Expected<std::optional<uint64_t>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event,
         ProcessResult &Result);
  /// KSM eligibility only; the deterministic profile has no background scanner.
  bool isMergeable(uint64_t Address) const;

private:
  AddressSpace &Space;
  const uint64_t PageSize, UserLimit, Limit, MinimumBreak, GuardBase, GuardEnd;
  uint64_t ProgramBreak;
  // Sorted, disjoint eligible intervals [begin, end). These own neither RAM
  // nor mappings; unmapping and new anonymous allocations retire the policy.
  std::vector<std::pair<uint64_t, uint64_t>> MergeableRanges;

  void setMergeable(uint64_t Address, uint64_t Size, bool Mergeable);
  std::optional<uint64_t> roundSize(uint64_t Size) const;
  bool validRange(uint64_t Address, uint64_t Size) const;
  llvm::Expected<std::vector<AddressMapping>> reservedRanges() const;
  std::optional<uint64_t> findGap(llvm::ArrayRef<AddressMapping> Ranges,
                                  uint64_t Start, uint64_t Size) const;
  llvm::Expected<bool> mapPages(uint64_t Address, uint64_t Size,
                                unsigned Permissions);
  llvm::Expected<bool> unmapPages(uint64_t Address, uint64_t Size);
  llvm::Expected<uint64_t> map(const ProcessServiceEvent &Event);
  llvm::Expected<uint64_t> protect(const ProcessServiceEvent &Event);
  llvm::Expected<uint64_t> unmap(const ProcessServiceEvent &Event);
  llvm::Expected<uint64_t> advise(const ProcessServiceEvent &Event);
  llvm::Expected<uint64_t> setBreak(uint64_t Address);
};
} // namespace neverd::emulation::linux_model
#endif
