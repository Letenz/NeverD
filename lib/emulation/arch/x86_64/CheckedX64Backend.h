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

namespace neverd::emulation {
class CheckedX64Backend final : public CheckedBackend {
public:
  static llvm::Expected<std::unique_ptr<ExecutionBackend>>
  create(ExecutionBackendKind Kind, uint64_t Limit);
  GuestArchitecture architecture() const override {
    return GuestArchitecture::X64;
  }
  llvm::Expected<RegisterValue> readRegister(CPURegister) override;
  llvm::Error writeRegister(CPURegister, const RegisterValue &) override;
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override;
  llvm::Error saveContext(BackendContext &) override;
  llvm::Error restoreContext(const BackendContext &) override;

private:
  CheckedX64Backend() : CheckedBackend(x64::MaxInstructionBytes, 1) {}
  bool canonicalRange(uint64_t, uint64_t) const override;
  uint64_t programCounter() const override { return CPU.reg(X64Register::PC); }
  void setProgramCounter(uint64_t PC) override {
    CPU.reg(X64Register::PC) = PC;
  }
  struct SavedState : BackendContext::Storage {
    X64MachineState CPU;
  };
  llvm::Error execute(const cs_insn &Instruction) override;
  llvm::Expected<uint64_t> operandRegister(unsigned Register) const;
  std::unique_ptr<X64Machine> Machine;
  X64MachineState CPU;
  uint64_t PageTableRoot = 0;
};
} // namespace neverd::emulation
#endif
