//===- CheckedX64Backend.h - Checked x64 execution-----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_CHECKEDX64BACKEND_H
#define NEVERD_EMULATION_ARCH_CHECKEDX64BACKEND_H
#include "../../core/CheckedBackend.h"
#include "X64Machine.h"

#include "neverd/emulation/ExecutionBackend.h"

#include <atomic>
#include <capstone/capstone.h>
#include <vector>

namespace neverd::emulation {
class CheckedX64Backend final : public CheckedBackend {
public:
  static llvm::Expected<std::unique_ptr<ExecutionBackend>>
  create(std::unique_ptr<MemoryProjection> Memory,
         std::unique_ptr<X64Machine> Machine, bool UserMode = false,
         bool SIMDExceptions = false);
  GuestArchitecture architecture() const override {
    return GuestArchitecture::X64;
  }
  bool supportsSIMDExceptions() const override { return SIMDExceptions; }
  std::optional<X64BranchModel> x64BranchModel() const override {
    return Machine->branchModel();
  }
  llvm::Expected<RegisterValue>
      supportedControlBits(CPURegister) const override;
  llvm::Expected<RegisterValue> readRegister(CPURegister) override;
  llvm::Error writeRegister(CPURegister, const RegisterValue &) override;
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override;
  llvm::Error saveContext(BackendContext &) override;
  llvm::Error restoreContext(const BackendContext &) override;
  llvm::Error mapMMIO(uint64_t, uint64_t, GuestMMIOCallbacks) override;
  llvm::Error unmapMMIO(uint64_t, uint64_t) override;
  bool hasDeviceError() const override { return DeviceFailed; }

private:
  CheckedX64Backend(bool UserMode, bool SIMDExceptions)
      : CheckedBackend(x64::MaxInstructionBytes, 1, UserMode),
        SIMDExceptions(SIMDExceptions) {}
  bool permitsMXCSR(uint64_t Value) const {
    return supportsSIMDExceptions() ||
           (Value & x64::InitialMXCSR) == x64::InitialMXCSR;
  }
  bool canonicalRange(uint64_t, uint64_t) const override;
  bool supportsDeviceMappings() const override { return !UserMode; }
  uint64_t programCounter() const override { return CPU.reg(X64Register::PC); }
  void setProgramCounter(uint64_t PC) override {
    CPU.reg(X64Register::PC) = PC;
    // A public entry resumes from the published architectural state. A REP
    // fault restores the flags at the start of this uninterrupted execution.
    StringRestart.reset();
  }
  struct SavedState : BackendContext::Storage {
    X64MachineState CPU;
  };
  llvm::Error execute(const cs_insn &Instruction) override;
  std::optional<ServiceRequest>
  decodeServiceRequest(const cs_insn &) const override;
  llvm::Expected<uint64_t> operandRegister(unsigned Register) const;
  llvm::Expected<uint64_t> operandAddress(const cs_insn &, const cs_x86_op &,
                                          uint64_t Offset = 0) const;
  struct Access {
    uint64_t Address;
    unsigned Size, Permission;
    uint64_t Value;
    std::optional<unsigned> Update = std::nullopt;
    uint64_t High = 0;
    bool Deferred = false;
  };
  llvm::Error prepareStack(const cs_insn &, bool Push,
                           std::vector<Access> &) const;
  void setOperandRegister(unsigned Register, uint64_t Value);
  std::shared_ptr<MemoryProjection::Device> deviceAt(uint64_t Address) const;
  llvm::Error validateDevice(const MemoryProjection::Device &, uint64_t,
                             unsigned, bool);
  llvm::Error deviceTransfer(const cs_insn &, uint64_t, unsigned, unsigned,
                             uint64_t);
  enum class StringOperation { Move, Store, Load, Compare, Scan };
  struct StringRestartState {
    uint64_t PC, Flags;
  };
  std::optional<StringRestartState> StringRestart;
  llvm::Error executeString(const cs_insn &, unsigned Size, StringOperation);
  llvm::Error deviceResult(llvm::Error E);
  template <typename Function> auto deviceCallback(Function Call) {
    try {
      return Call();
    } catch (...) {
      DeviceFailed = true;
      throw;
    }
  }
  std::unique_ptr<X64Machine> Machine;
  X64MachineState CPU;
  const bool SIMDExceptions;
  bool DeviceFailed = false;
};
} // namespace neverd::emulation
#endif
