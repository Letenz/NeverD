//===- WindowsProcessTestSupport.h - Process fixtures -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_WINDOWSPROCESSTESTSUPPORT_H
#define NEVERD_UNITTESTS_EMULATION_WINDOWSPROCESSTESTSUPPORT_H

#include "neverd/emulation/ProcessObserver.h"

namespace neverd::emulation::test {
/// Measure startup mappings independently of the provider catalogue's layout.
/// Resource-reclamation tests add a small transient allowance to this value;
/// a larger catalogue must not consume the allowance being tested.
inline llvm::Expected<uint64_t>
startupMappedBytes(const std::filesystem::path &Path,
                   const ProcessOptions &Options) {
  struct Observer final : ProcessObserver {
    uint64_t Bytes = 0;
    llvm::Expected<std::vector<ExecutionWatch>>
    started(ProcessView &Process) override {
      auto Mappings = Process.mappings();
      if (!Mappings)
        return Mappings.takeError();
      for (const auto &M : *Mappings)
        if (!M.Device)
          Bytes += M.Size;
      auto PC =
          Process.readRegister(Process.architecture() == GuestArchitecture::X64
                                   ? CPURegister::X64PC
                                   : CPURegister::AArch64PC);
      if (!PC)
        return PC.takeError();
      return std::vector<ExecutionWatch>{{(*PC)[0], 1}};
    }
    llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
    watched(ProcessView &, uint64_t) override {
      return std::nullopt;
    }
  } Observer;
  auto Result =
      observeProcess(Path, ProcessProfile::WindowsPE64, Options, Observer);
  if (!Result)
    return Result.takeError();
  if (Result->Stop != ProcessStopReason::Observer)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "startup observation failed: %s",
                                   Result->Diagnostic.c_str());
  return Observer.Bytes;
}
} // namespace neverd::emulation::test
#endif
