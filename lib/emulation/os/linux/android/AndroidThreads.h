//===- AndroidThreads.h - Cooperative guest-thread ownership ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDTHREADS_H
#define NEVERD_EMULATION_ANDROIDTHREADS_H
#include "AndroidInternal.h"

#include <deque>

namespace neverd::emulation::android_model {
namespace thread_model {
#define NEVERD_ANDROID_THREAD_VALUE(Name, Value)                               \
  inline constexpr uint64_t Name = Value;
#include "AndroidThreads.def"
#undef NEVERD_ANDROID_THREAD_VALUE
} // namespace thread_model
struct PendingCallback {
  GuestCallback Call;
  ServiceRequest Request;
  size_t Event;
  uint64_t Link, StackPointer;
};

/// One shared CPU transport, with complete saved CPU contexts at consumed
/// service/quantum boundaries. Guest RAM is never rolled back on a switch.
/// Bionic process state is shared; TLS, waits and callbacks belong to threads.
class GuestThreads {
public:
  GuestThreads(ExecutionBackend &CPU, const IntegerABI &Calls,
               const ProcessOptions &Options, ProcessResult &Result)
      : CPU(CPU), Calls(Calls), Options(Options), Result(Result) {}
  llvm::Error initialize(uint64_t StackBase);
  llvm::Error validateTLS();
  bool enabled() const { return Options.Android->ThreadLimit > 1; }
  uint64_t id() const { return current().Kernel.ID; }
  uint64_t tls() const { return current().Report.TLS; }
  bool isEntry() const { return Current == 0; }
  linux_model::ThreadContext *kernel() {
    return enabled() ? &current().Kernel : nullptr;
  }
  std::vector<PendingCallback> &callbacks() { return current().Callbacks; }
  void setReturnSP(uint64_t SP) { current().ReturnSP = SP; }
  uint64_t returnSP() const { return current().ReturnSP; }
  BionicResult invoke(const NativeCallEvent &Call);
  void suspend(const ServiceRequest &Request, size_t Event);
  llvm::Error finish(uint64_t Value, uint32_t ExitStatus = 0);
  /// Round robin, including the current thread if it is the only runnable
  /// owner. False classifies all-finished or a join cycle in Result.
  llvm::Expected<bool> schedule();
  void report();

private:
  struct Join {
    size_t Target;
    uint64_t Output;
    std::optional<ServiceRequest> Request;
    size_t Event = 0;
  };
  struct Thread {
    NativeThreadSnapshot Report;
    linux_model::ThreadContext Kernel;
    std::array<uint8_t, thread_attribute_abi::ObjectBytes> Attributes{};
    std::unique_ptr<BackendContext> Context;
    std::vector<PendingCallback> Callbacks;
    std::optional<Join> Waiting;
    std::optional<size_t> Joiner;
    uint64_t MappingBase = 0, MappingSize = 0, ReturnSP = 0;
  };
  ExecutionBackend &CPU;
  const IntegerABI &Calls;
  const ProcessOptions &Options;
  ProcessResult &Result;
  std::deque<Thread> Threads;
  size_t Current = 0;
  uint32_t LastExitStatus = 0;
  Thread &current() { return Threads[Current]; }
  const Thread &current() const { return Threads[Current]; }
  llvm::Error access(uint64_t Address, uint64_t Size, unsigned Permissions,
                     unsigned Alignment = 8);
  llvm::Error put64(uint64_t Address, uint64_t Value);
  llvm::Error retire(size_t Index);
  llvm::Error completeJoin(Thread &Waiter);
  BionicResult create(const NativeCallEvent &Call);
  BionicResult unsupported(llvm::StringRef Reason);
};
} // namespace neverd::emulation::android_model
#endif
