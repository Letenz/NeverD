//===- StateSourceRecovery.h - Explicit state ABI source contracts -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover ordinary source calls from an explicitly isolated register bank.

#ifndef NEVERD_PIPELINE_STATESOURCERECOVERY_H
#define NEVERD_PIPELINE_STATESOURCERECOVERY_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Module;
}

namespace neverd {
struct StateSourceSlot {
  unsigned Offset = 0;
  unsigned Bytes = 8;
};
struct StateSourceWrite {
  StateSourceSlot Slot;
  uint64_t Value = 0;
};
enum class StateSourceType { Void, U32, U64, I32, I64, Pointer };
struct StateSourceImport {
  uint64_t Selector = 0;
  std::string Name;
  std::vector<StateSourceType> Parameters;
  StateSourceType Result = StateSourceType::U64;
};
struct StateSourceGateway {
  std::string Name;
  std::optional<StateSourceSlot> Selector;
  std::optional<StateSourceSlot> ArgumentCount;
  std::vector<StateSourceSlot> Arguments;
  StateSourceSlot ResultOffset;
  StateSourceSlot ResultBytes;
  std::vector<StateSourceWrite> Writes;
  std::vector<StateSourceImport> Imports;
};
struct StateSourceContract {
  unsigned StateBytes = 0;
  /// Required assertions, supplied separately from path observations:
  /// guest/context memory cannot alias the register bank; calls cannot retain
  /// bank addresses; declared gateways return zero status after normal return
  /// and modify only the declared result channel and fixed writes in the bank.
  /// Selector, arguments and result-channel descriptors are captured on entry,
  /// before the fixed writes and imported operation.
  /// Imported callees may freely access guest memory and may fail to return.
  bool IsolatedBank = false;
  bool ZeroStatusGateways = false;
  std::vector<StateSourceGateway> Gateways;
};
struct StateSourceRecovery {
  StateSourceContract Contract;
  std::vector<std::string> Probes;
  unsigned Functions = 0;
  unsigned RewrittenCalls = 0;
  unsigned GuardedCalls = 0;
  unsigned UnresolvedCalls = 0;
};

/// Validate the entire contract/module before mutation. Scoped alias facts
/// keep the public state ABI and unresolved call boundaries intact. Temporary
/// probes retain ordinary call effects until finishStateSourceRecovery.
llvm::Expected<StateSourceRecovery>
prepareStateSourceRecovery(llvm::Module &Module,
                           const StateSourceContract &Contract);

/// Consume constants proven by the shared LLVM optimizer at each probe.
/// An unknown argument count gets a runtime guard and the original fallback.
/// Restore unresolved gateways. No observation name or sampled value is used.
llvm::Error finishStateSourceRecovery(llvm::Module &Module,
                                      StateSourceRecovery &Recovery);
} // namespace neverd
#endif
