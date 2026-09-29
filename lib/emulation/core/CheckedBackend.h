//===- CheckedBackend.h - Shared checked execution lifecycle -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CHECKEDBACKEND_H
#define NEVERD_EMULATION_CHECKEDBACKEND_H
#include "MemoryProjection.h"

#include "neverd/emulation/CPU.h"

#include <atomic>
#include <capstone/capstone.h>
#include <chrono>
namespace neverd::emulation {
/// Shared pre-effect fault, observer, memory and bounded execution semantics.
/// ISA admission and architectural state belong to the derived architecture.
class CheckedBackend : public ExecutionBackend {
public:
  ~CheckedBackend() override;
  std::shared_ptr<AddressSpace> addressSpace() const override {
    return Memory->addressSpace();
  }
  llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace> Space) override;
  llvm::Error map(uint64_t, uint64_t, unsigned) override;
  llvm::Error mapAlias(uint64_t, uint64_t, uint64_t, unsigned) override;
  llvm::Error unmapAlias(uint64_t, uint64_t) override;
  llvm::Error replaceAliases(llvm::ArrayRef<GuestAliasRange>,
                             llvm::ArrayRef<GuestAliasMapping>) override;
  llvm::Error protect(uint64_t, uint64_t, unsigned) override;
  llvm::Error read(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error write(uint64_t, llvm::ArrayRef<uint8_t>) override;
  llvm::Error fetch(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Expected<bool> canAccess(uint64_t, uint64_t, unsigned) const override;
  llvm::Error validateBacking(uint64_t, uint64_t) const override;
  llvm::Error readBacking(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error writeBacking(uint64_t, llvm::ArrayRef<uint8_t>) override;
  llvm::Error snapshotBacking(uint64_t,
                              llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error installHooks(BackendHooks) override;
  llvm::Error run(uint64_t, uint64_t) override;
  void stop() override { StopRequested.store(true); }
  bool timedOut() const override { return TimedOut; }
  bool hasMemoryFault() const override {
    return FirstFault && FirstFault->Access.has_value();
  }
  bool hasDeviceError() const override { return false; }
  bool executable(uint64_t Address) const override {
    auto Result = Memory->addressSpace()->canAccess(Address, 1, Execute);
    if (!Result) {
      llvm::consumeError(Result.takeError());
      return false;
    }
    return *Result;
  }
  std::optional<BackendFault> fault() const override { return FirstFault; }
  std::optional<BackendFault> takeRecoverableFault() override;

protected:
  CheckedBackend(unsigned MaxInstructionBytes, unsigned InstructionAlignment)
      : MaxInstructionBytes(MaxInstructionBytes),
        InstructionAlignment(InstructionAlignment) {}
  llvm::Error initializeDecoder(cs_arch, cs_mode);
  virtual bool canonicalRange(uint64_t, uint64_t) const = 0;
  virtual uint64_t programCounter() const = 0;
  virtual void setProgramCounter(uint64_t PC) = 0;
  virtual llvm::Error execute(const cs_insn &) = 0;
  llvm::Error mutableMemory() const;
  llvm::Error access(uint64_t, uint64_t, unsigned, bool Recoverable = false);
  std::unique_ptr<MemoryProjection> Memory;
  BackendHooks Hooks;
  csh Decoder = 0;
  std::shared_ptr<const void> Identity = std::make_shared<unsigned char>(0);
  std::optional<BackendFault> FirstFault, RecoverableFault;
  bool Running = false, TimedOut = false;
  std::atomic<bool> StopRequested{false};
  std::chrono::steady_clock::time_point Deadline;

private:
  unsigned MaxInstructionBytes, InstructionAlignment;
};
} // namespace neverd::emulation
#endif
