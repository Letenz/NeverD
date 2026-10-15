//===- DriverObservation.h - Stopped driver inspection ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DRIVEROBSERVATION_H
#define NEVERD_EMULATION_DRIVEROBSERVATION_H

#include "../kernel/KernelExportRegistry.h"

#include "neverd/emulation/MemoryView.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/WindowsDriverState.h"

namespace neverd::emulation {
struct DriverImage;
struct DriverResult;
class KernelModel;

/// The driver environment owns invocation provenance and object lifetimes.
/// All observer callbacks run while the CPU is stopped; instruction hooks
/// only request a suspension.
class DriverObservation final : public ProcessView {
public:
  struct RelocationWitness {
    MemoryView Bytes;
    MemoryWriteWitness Writes;
  };
  /// Bind loader fields immediately after image copy, before model fixups.
  static llvm::Expected<std::vector<RelocationWitness>>
  watchRelocations(ExecutionBackend &CPU, const DriverImage &Image);
  DriverObservation(ProcessObserver *Observer, ExecutionBackend &CPU,
                    const DriverImage &Image, KernelModel &Kernel,
                    const KernelExportRegistry &Exports,
                    const DriverResult &Result,
                    std::vector<RelocationWitness> RelocationWrites,
                    std::string Name);
  llvm::Error entered(bool EntryInvocation, ProcessStackView Stack);
  llvm::Error resuming();
  void setExecutionGates(std::vector<ExecutionWatch> Gates);
  bool matches(uint64_t PC, bool Direct = false);
  /// Consume a resumed watch only after an instruction is admitted. A time
  /// slice or environment setup may otherwise stop before it can execute.
  void instructionAdmitted();
  /// A free processor interval has no per-instruction admission history.
  void opaqueExecutionResuming();
  /// True resumes once at PC; false ends the observed workload.
  llvm::Expected<bool> watched(uint64_t PC);
  llvm::Error exporting(const KernelExportRegistry::Export &Export);

  GuestArchitecture architecture() const override;
  llvm::Expected<RegisterValue> readRegister(CPURegister Register) override;
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override;
  llvm::Expected<std::vector<AddressMapping>> mappings() override;
  std::vector<ProcessModuleView> modules() override;
  std::vector<ProcessExportView> exports() override;
  llvm::Expected<std::optional<ProcessCallFrame>> callFrame() override;
  bool programInvocation() const override { return EntryInvocation; }
  std::optional<ProcessStackView> stack() const override { return Stack; }
  std::optional<std::vector<ProcessHeapAllocationView>>
  heapAllocations() const override;
  std::optional<std::vector<uint64_t>> encodedPointers() const override {
    return std::vector<uint64_t>();
  }
  llvm::Expected<std::optional<ProcessDynamicThreadLocalState>>
  dynamicThreadLocalState() override {
    return ProcessDynamicThreadLocalState{};
  }
  llvm::Expected<std::optional<std::vector<uint8_t>>>
  threadLocalMemory() override {
    return std::vector<uint8_t>();
  }
  llvm::Expected<std::shared_ptr<const ProcessRuntimeState>>
  runtimeState(bool IncludeBacking) override;
  std::optional<uint64_t> nativeCallCount() const override;
  llvm::Expected<uint32_t> instructionSize(uint64_t Address) override;
  bool watchedMemoryUnchanged() const override { return MemoryUnchanged; }
  void setWatchedMemoryUnchanged(bool Unchanged) {
    MemoryUnchanged = Unchanged;
  }

private:
  llvm::Error replaceWatches(std::vector<ExecutionWatch> Next);
  llvm::Error installExecutionWatches();
  llvm::Expected<WindowsDriverState::RegisterFile> entryRegisters();
  llvm::Expected<WindowsDriverState::EntryContext>
  entryContext(bool IncludeBacking);
  llvm::Expected<WindowsDriverState::ImageRelocationState> imageRelocations();
  ProcessObserver *Observer;
  ExecutionBackend &CPU;
  const DriverImage &Image;
  KernelModel &Kernel;
  const KernelExportRegistry &Exports;
  const DriverResult &Result;
  std::vector<RelocationWitness> RelocationWrites;
  std::string Name;
  bool Started = false, EntryInvocation = false;
  bool MemoryUnchanged = false;
  bool ExecutedPrefix = false;
  ProcessStackView Stack{};
  WindowsDriverState::RegisterFile InitialRegisters;
  std::array<uint8_t, 40> InitialFrame{};
  std::optional<uint64_t> ResumePC;
  std::vector<ExecutionWatch> Watches;
  std::vector<ExecutionWatch> ExecutionGates;
  bool ExecutionWatchesDirty = false;
};
} // namespace neverd::emulation
#endif
