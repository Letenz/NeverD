//===- GuestInvocation.h - Finite scalar guest calls ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finite scalar guest calls.
///

#ifndef NEVERD_EMULATION_GUESTINVOCATION_H
#define NEVERD_EMULATION_GUESTINVOCATION_H
#include "neverd/emulation/ExecutionSession.h"
#include "neverd/emulation/IntegerABI.h"

namespace neverd::emulation {
struct GuestInvocationArgument {
  /// A scalar, or a byte offset into Buffers[*Buffer]. One-past pointers are
  /// allowed; no guessed ABI classification, extension or hidden parameters.
  uint64_t Value = 0;
  std::optional<uint32_t> Buffer;
};
struct GuestInvocationBuffer {
  uint64_t Size = 0;
  std::vector<uint8_t> Input;
  bool Capture = true;
};
struct GuestInvocationRequest {
  /// Empty means an absolute guest address. Otherwise Entry is an RVA in this
  /// declared/mapped instance, never an original file address.
  std::string InstanceID;
  uint64_t Entry = 0;
  std::optional<IntegerCallingConvention> ABI;
  std::vector<GuestInvocationArgument> Arguments;
  std::vector<GuestInvocationBuffer> Buffers;
  ExecutionLimits Limits{1000000, 10000, 1000000};
};
struct GuestInvocationPlan {
  /// Environment-owned, private, already mapped RAM. The environment retains
  /// its lifetime through all yields and ensures no aliases of live callers.
  uint64_t StackBase = 0, StackSize = 0, ReturnPC = 0;
};
struct GuestInvocationFrame {
  uint64_t Entry, ReturnPC, StackBase, StackSize, StackPointer,
      ReturnStackPointer;
};
struct GuestInvocationProgress {
  GuestInvocationFrame Frame;
  uint64_t Instructions, Events, Microseconds, RemainingInstructions,
      RemainingEvents, RemainingMicroseconds;
};
struct GuestInvocationOutput {
  uint64_t Address;
  std::vector<uint8_t> Bytes;
};
struct GuestInvocationResult {
  uint64_t Entry = 0, Value = 0;
  uint64_t Instructions = 0, Events = 0, Microseconds = 0;
  /// One row per input buffer; uncaptured rows retain their address but no
  /// bytes.
  std::vector<GuestInvocationOutput> Buffers;
};

/// ABI/frame/buffer and caller-control ownership only; never runs a CPU or
/// dispatches an OS model. ExecutionRun and the same environment pump the call.
class GuestInvocation final {
public:
  ~GuestInvocation() = default;

private:
  friend class ExecutionRun;
  struct AccountingStart {
    uint64_t Instructions, Events, RemainingMicroseconds;
  };
  static llvm::Error validate(const GuestInvocationRequest &, GuestArchitecture,
                              uint64_t ByteLimit);
  /// Pure stopped preflight; invalid requests/plans have no guest effects.
  static llvm::Expected<std::unique_ptr<GuestInvocation>>
  prepare(GuestInvocationRequest, GuestInvocationPlan, uint64_t Entry,
          ExecutionSession &, uint64_t ByteLimit, AccountingStart);
  /// After install begins, any error must terminate the owning workload.
  llvm::Error install(ExecutionSession &);
  llvm::Expected<GuestInvocationResult> result(ExecutionSession &) const;
  llvm::Error restoreCaller(ExecutionSession &);
  GuestInvocationProgress progress(const ExecutionBudget &) const;
  const GuestInvocationFrame &frame() const { return Frame; }

  GuestInvocation(GuestInvocationRequest, IntegerABI, GuestInvocationFrame,
                  uint64_t PayloadSize, std::vector<uint64_t> Addresses,
                  AccountingStart);
  GuestInvocationRequest Request;
  IntegerABI ABI;
  GuestInvocationFrame Frame;
  uint64_t PayloadSize, StartInstructions, StartEvents, StartMicroseconds;
  std::vector<uint64_t> Addresses;
  std::unique_ptr<SessionContext> Caller;
};
} // namespace neverd::emulation
#endif
