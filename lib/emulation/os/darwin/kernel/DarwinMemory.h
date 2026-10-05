//===- DarwinMemory.h - Darwin virtual memory ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINMEMORY_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINMEMORY_H
#include "DarwinKernel.h"

#include "neverd/emulation/AddressSpace.h"

namespace neverd::emulation::darwin_model {
class DarwinFiles;
/// The address space owns live mappings. This model owns only OS reservations
/// and maximum protection, which cannot be inferred from current permissions.
class DarwinMemory {
public:
  DarwinMemory(AddressSpace &Space, const MemoryLayout &Layout,
               const ProcessOptions &Options);
  llvm::Expected<std::optional<ServiceResult>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event, DarwinFiles &Files,
         ProcessResult &Result);

private:
  AddressSpace &Space;
  uint64_t PageSize, Minimum, Limit, GuardBase;
  std::vector<MaximumProtection> Maximum;
  llvm::Expected<std::optional<ServiceResult>>
  map(const ProcessServiceEvent &Event, DarwinFiles &Files,
      ProcessResult &Result);
  llvm::Expected<ServiceResult> protect(const ProcessServiceEvent &Event);
  llvm::Expected<ServiceResult> unmap(const ProcessServiceEvent &Event);
  void forgetMaximum(uint64_t Address, uint64_t Size);
};
} // namespace neverd::emulation::darwin_model
#endif
