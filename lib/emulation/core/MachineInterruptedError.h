//===- MachineInterruptedError.h - Acknowledged transport interruption ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_MACHINEINTERRUPTEDERROR_H
#define NEVERD_EMULATION_CORE_MACHINEINTERRUPTEDERROR_H

#include "llvm/Support/Error.h"

namespace neverd::emulation {
/// An acknowledged interruption has no publishable CPU/RAM progress. It is
/// distinct from a failed host entry, state transfer or guest exception.
class MachineInterruptedError final
    : public llvm::ErrorInfo<MachineInterruptedError> {
public:
  static inline char ID = 0;
  MachineInterruptedError(const char *Text, bool Stopped, bool Expired)
      : Text(Text), Stopped(Stopped), Expired(Expired) {}
  bool stopRequested() const { return Stopped; }
  bool deadlineReached() const { return Expired; }
  void log(llvm::raw_ostream &OS) const override { OS << Text; }
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }

private:
  const char *Text;
  bool Stopped, Expired;
};
} // namespace neverd::emulation
#endif
