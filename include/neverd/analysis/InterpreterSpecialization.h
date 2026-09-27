//===- InterpreterSpecialization.h - LowIR partial evaluation ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETERSPECIALIZATION_H
#define NEVERD_ANALYSIS_INTERPRETERSPECIALIZATION_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/symbolic/SymState.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::analysis {

struct SpecializationCursor {
  va_t Address = 0;
  InstructionMode Mode = InstructionMode::Default;

  bool operator==(const SpecializationCursor &) const = default;
};

/// A complete, strictly lifted guest instruction. The provider owns decoding,
/// mapping, relocation, and instruction-level exception checks. A missing or
/// unsupported instruction must return an Error, never an empty approximation.
struct SpecializationInstruction {
  std::vector<LowOp> Ops;
  LowInstructionBoundary Origin;
  SpecializationCursor Fallthrough;
};

/// An exact non-faulting ordinary read whose bytes remain immutable throughout
/// every supported execution. This is a provider certificate, not a snapshot
/// sampled from writable memory. The returned byte count must equal the
/// request.
struct SpecializationImmutableRead {
  std::vector<uint8_t> Bytes;
  std::string Evidence;
};

class SpecializationProvider {
public:
  virtual ~SpecializationProvider() = default;
  virtual llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) = 0;
  virtual std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) {
    return std::nullopt;
  }
};

struct SpecializationConstant {
  NdVar Location;
  uint64_t Value = 0;
};

struct SpecializationFrameSlot {
  /// Displacement from the entry value of FrameBaseRegister, never from its
  /// current value after a stack adjustment. Negative offsets are ordinary.
  int64_t Offset = 0;
  uint16_t Bytes = 0;
};

struct SpecializationOptions {
  /// A context contains only these control ranges. Unknown is a valid context
  /// component, not an assumed zero. All other constant bytes join by
  /// intersection, so changing business values do not unroll a loop forever.
  std::vector<symbolic::SymRegisterRange> ControlRegisters;
  /// Optional entry-relative frame identity. This enables affine pointer and
  /// constant frame-byte propagation, not a private/non-aliasing memory claim.
  /// The initial implementation requires an eight-byte register root.
  std::optional<symbolic::SymRegisterRange> FrameBaseRegister;
  /// Context hints only: missing bytes remain unknown. Other frame facts join
  /// by intersection, so changing spilled business values do not unroll loops.
  std::vector<SpecializationFrameSlot> ControlFrameSlots;
  /// Native RETURN is accepted only with the original frame-base value and
  /// without writes to its entry [0, 8) control slot. Requires a frame root.
  /// This excludes push/RET dispatch until explicit return-target lifting
  /// exists.
  bool RequireRestoredFrameAtReturn = false;
  /// Explicit source-ABI precondition: every external-origin store target
  /// range, including computed external addresses, is disjoint from the entry
  /// control slot. Frame-derived or unknown-origin addresses are never
  /// exempted by this precondition.
  bool ExternalStoresPreserveEntryReturnSlot = false;
  /// Preconditions supplied by the caller, not values inferred from a run.
  std::vector<SpecializationConstant> EntryConstants;
  llvm::endianness ByteOrder = llvm::endianness::little;
  uint32_t MaxNodes = 4096;
  uint32_t MaxContextsPerAddress = 64;
  uint64_t MaxOperations = 262144;
  uint32_t MaxNodeEvaluations = 16384;
  uint32_t MaxIndirectTargets = 16;
};

enum class SpecializationStatus : uint8_t {
  Complete,
  InvalidInput,
  Unsupported,
  UnresolvedControl,
  BudgetExceeded,
};

struct SpecializationOrigin {
  va_t ResidualAddress = 0;
  LowInstructionBoundary NativeInstruction;
};

struct SpecializationReadWitness {
  va_t InstructionAddress = 0;
  int OpSeq = 0;
  va_t Address = 0;
  std::vector<uint8_t> Bytes;
  std::string Evidence;
};

struct SpecializationResult {
  SpecializationStatus Status = SpecializationStatus::InvalidInput;
  /// Populated only on Complete. Incomplete exploration is never a replacement.
  LowFunc Residual;
  std::vector<SpecializationOrigin> Origins;
  std::vector<SpecializationReadWitness> Reads;
  std::string Diagnostic;
  uint64_t EvaluatedOperations = 0;
  uint32_t NodeEvaluations = 0;
  uint32_t Contexts = 0;

  bool complete() const { return Status == SpecializationStatus::Complete; }
};

/// Provider-neutral partial evaluation of strictly lifted integer/control
/// LowIR. Dynamic operations and ordinary memory effects remain in the residual
/// CFG. Calls, opaque semantics, ordered memory, and unsupported instruction
/// guards are refused. Synthetic labels identify clones; NativeInstruction
/// preserves provenance without copying stale address-occurrence certificates.
SpecializationResult
specializeInterpreter(SpecializationProvider &Provider,
                      SpecializationCursor Entry,
                      const SpecializationOptions &Options = {});

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_INTERPRETERSPECIALIZATION_H
