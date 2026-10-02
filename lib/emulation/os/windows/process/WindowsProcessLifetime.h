//===- WindowsProcessLifetime.h - Module lifetimes ----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_LIFETIME_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_LIFETIME_H
#include "WindowsProcessModules.h"

namespace neverd::emulation::windows_process {
class Lifetime final {
public:
  enum class CallKind { TLS, DLL, Entry };
  struct Call {
    CallKind Kind;
    uint64_t PC, ReturnGate;
    std::vector<uint64_t> Arguments;
  };
  explicit Lifetime(const Program &Program);
  llvm::Expected<std::optional<Call>> next(ExecutionBackend &CPU);
  llvm::Error returned(uint64_t Value);
  llvm::Error exit(uint32_t Status);
  std::optional<uint32_t> exitStatus() const { return ExitStatus; }

private:
  struct Notification {
    size_t Module;
    CallKind Kind;
    uint64_t Reason;
  };
  enum class ModuleState { Pending, TLS, Entry, Attached };
  llvm::Error beginExit(uint32_t Status, bool InitializationFailed);
  void advance();
  const Program &Program;
  std::vector<ModuleState> States;
  std::vector<Notification> Pending;
  size_t Position = 0, Callback = 0;
  std::optional<uint64_t> CallbackArray;
  bool Running = false, Detaching = false;
  std::optional<uint32_t> ExitStatus;
};
} // namespace neverd::emulation::windows_process
#endif
