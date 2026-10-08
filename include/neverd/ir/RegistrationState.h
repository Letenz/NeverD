//===- RegistrationState.h - Checked x86 EH state flow -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_REGISTRATIONSTATE_H
#define NEVERD_IR_REGISTRATIONSTATE_H

#include "neverd/loader/ExceptionCommon.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct LowFunc;

/// Reaching registration states at an exact, decoded LowIR block. These are
/// CFG facts, not an IP-to-state table in the input image. Keep them separate
/// from the loader descriptor authenticated by the rewrite transaction.
struct RegistrationBlockState {
  int BlockId = -1;
  ExceptionAddressRange Range;
  std::vector<int32_t> Levels;
  bool Unknown = false;
  /// Runtime-invoked filter/cleanup code has its own dispatch context. Its
  /// instructions must not be included in the parent's lexical try interval.
  bool CallbackOnly = false;
  /// Finally callbacks can dispatch to outer scopes even though they are not
  /// lexical parent blocks. Searching filters have a separate runtime context.
  bool CanDispatch = false;
};

/// An exact LowIR operation that accesses the runtime chain head. Native
/// lowering may replace chain administration only by matching these source
/// occurrence identities; a function-wide completeness bit is insufficient.
struct RegistrationChainAccess {
  enum class Kind : uint8_t {
    ReadPreviousHead,
    ReadInstalledHead,
    Install,
    Remove,
  };
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  Kind AccessKind = Kind::ReadPreviousHead;
};

struct RegistrationStateAnalysis {
  std::vector<RegistrationBlockState> Blocks;
  bool Complete = false;
  bool CallbackStatesComplete = true;
  bool RegistrationLifetimeComplete = false;
  /// Complete chain access ownership, including decoded boundaries and
  /// operation identities, available only with a complete registration
  /// lifetime and callback state proof. This is not a native output receipt.
  bool ChainOperationsComplete = false;
  std::vector<RegistrationChainAccess> ChainAccesses;
  std::vector<std::string> Diagnostics;
};

/// Solve ordinary and runtime-dispatch state transfers together. Stores take
/// effect only after their exact decoded instruction retires. A union at a
/// join is retained; address order never selects a predecessor's state.
RegistrationStateAnalysis analyzeRegistrationStates(const LowFunc &Function);

/// Return exact address intervals only if every reaching state agrees about
/// membership. An ambiguous join cannot be flattened into a lexical try range.
template <typename Predicate>
std::optional<std::vector<ExceptionAddressRange>>
registrationRangesWhere(const RegistrationStateAnalysis &Analysis,
                        Predicate Contains) {
  if (!Analysis.Complete)
    return std::nullopt;
  std::vector<ExceptionAddressRange> Ranges;
  for (const RegistrationBlockState &Block : Analysis.Blocks) {
    if (Block.CallbackOnly)
      continue;
    if (Block.Unknown)
      return std::nullopt;
    if (Block.Levels.empty())
      continue;
    const bool Included = Contains(Block.Levels.front());
    for (int32_t Level : Block.Levels)
      if (Contains(Level) != Included)
        return std::nullopt;
    if (Included)
      Ranges.push_back(Block.Range);
  }
  std::sort(Ranges.begin(), Ranges.end(),
            [](const ExceptionAddressRange &A, const ExceptionAddressRange &B) {
              return A.Begin < B.Begin;
            });
  std::vector<ExceptionAddressRange> Merged;
  for (const ExceptionAddressRange &Range : Ranges) {
    if (!Range.isValid() ||
        (!Merged.empty() && Range.Begin < Merged.back().End))
      return std::nullopt;
    if (!Merged.empty() && Merged.back().End == Range.Begin)
      Merged.back().End = Range.End;
    else
      Merged.push_back(Range);
  }
  return Merged;
}

} // namespace neverd

#endif // NEVERD_IR_REGISTRATIONSTATE_H
