//===- WindowsNativeServices.h - Native export boundaries -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_NATIVE_SERVICES_H
#define NEVERD_EMULATION_WINDOWS_NATIVE_SERVICES_H
#include "WindowsProcess.h"

#include <array>

namespace neverd::emulation::windows_process {
/// Shared by the image producer and the execution-evidence checker.
std::array<uint8_t, value::NativeSyscallOffset>
nativeServicePrologue(uint32_t Number);

/// A matching register value is not evidence that an export was entered.
/// Witness both fixed prologue instructions before attributing its syscall.
class NativeEntryEvidence {
  std::map<uint64_t, uint32_t> Entries;
  std::optional<uint64_t> Entry, ExpectedPC, VerifiedPC;

public:
  explicit NativeEntryEvidence(llvm::ArrayRef<Import> Gates);
  std::vector<ExecutionWatch> watches() const;
  llvm::Error watched(ExecutionBackend &CPU, uint64_t PC);
  void invalidate();
  bool take(uint64_t PC);
};
} // namespace neverd::emulation::windows_process
#endif
