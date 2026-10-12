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
struct StateSourcePointerSource {
  unsigned Image = 0;
  unsigned Offset = 0;
};
struct StateSourceImport {
  uint64_t Selector = 0;
  std::string Name;
  std::vector<StateSourceType> Parameters;
  StateSourceType Result = StateSourceType::U64;
  /// Candidate identity for a pointer loaded from this image slot. This is
  /// never a constant-memory assertion: the captured target must also equal
  /// the imported function's address before a direct call is emitted.
  std::optional<StateSourcePointerSource> PointerSource;
};
struct StateSourceImage {
  StateSourceSlot Base;
  /// A readable guest extent, disjoint from the bank. Bytes are only a
  /// specialization candidate: their current contents are checked at runtime.
  bool Readable = false;
  std::vector<uint8_t> Bytes;
};
struct StateSourceString {
  unsigned Argument = 0;
  unsigned Image = 0;
  /// The gateway passes a NUL-terminated string by value. The imported API
  /// only reads its content, neither retaining its address nor observing its
  /// identity. Scratch allocation
  /// and normalization have no other observable effects under this contract.
  bool ByValue = false;
  /// When both slots are present, a nonzero key and matching mode select
  /// bytewise XOR normalization, including the encoded NUL terminator.
  std::optional<StateSourceSlot> Mode;
  std::optional<StateSourceSlot> XorKey;
  uint64_t EncodedMode = 1;
};
struct StateSourceArgumentExclusion {
  unsigned Argument = 0;
  uint64_t Value = 0;
};
struct StateSourceGateway {
  std::string Name;
  std::optional<StateSourceSlot> Selector;
  /// Mutually exclusive with Selector. Imports supply PointerSource instead
  /// of numeric selectors; unknown provenance or addresses keep the gateway.
  std::optional<StateSourceSlot> FunctionPointer;
  std::optional<StateSourceSlot> ArgumentCount;
  std::vector<StateSourceSlot> Arguments;
  StateSourceSlot ResultOffset;
  StateSourceSlot ResultBytes;
  std::vector<StateSourceWrite> Writes;
  std::vector<StateSourceImport> Imports;
  /// These argument slots contain offsets of eight-byte values in the bank.
  /// Only statically bounded offsets are dereferenced, before fixed writes.
  std::vector<unsigned> IndirectArguments;
  std::vector<StateSourceString> Strings;
  /// Values with additional gateway behavior retain the original call.
  std::vector<StateSourceArgumentExclusion> ExcludedArguments;
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
  std::vector<StateSourceImage> Images;
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
/// Argument counts and candidate strings get runtime guards and the original
/// fallback. Indirect arguments require statically bounded bank offsets.
/// Restore unresolved gateways. No observation name or sampled value is used.
llvm::Error finishStateSourceRecovery(llvm::Module &Module,
                                      StateSourceRecovery &Recovery);
} // namespace neverd
#endif
