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

/// Provider certificate for the omitted physical effects of a native near
/// CALL/RETURN. The instruction uses the configured eight-byte frame register
/// as its stack pointer. CALL pushes its exact fallthrough address; RETURN
/// consumes one address and has no additional stack adjustment. Generic LowIR
/// CALL/RETURN operands alone do not establish this machine-level contract.
/// A memory CALL retains a temporary-only effective-address prefix, one final
/// ordinary eight-byte LOAD, and an INDIR_CALL of that loaded temporary. The
/// entire target evaluation precedes the physical return-address push; a
/// legacy constant-slot INDIR_CALL is not a loaded target certificate.
enum class SpecializationNativeStackControl : uint8_t { None, Call, Return };

/// A complete, strictly lifted guest instruction. The provider owns decoding,
/// mapping, relocation, and instruction-level exception checks. A missing or
/// unsupported instruction must return an Error, never an empty approximation.
struct SpecializationInstruction {
  std::vector<LowOp> Ops;
  LowInstructionBoundary Origin;
  SpecializationCursor Fallthrough;
  SpecializationNativeStackControl NativeStackControl =
      SpecializationNativeStackControl::None;
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
  /// Preserve incoming machine flags as symbolic machine-state inputs rather
  /// than claiming they are supplied by an ordinary source-language ABI.
  bool ExplicitMachineState = false;
  /// Optional execution-profile certificates; neither is valid without the
  /// explicit machine-state interface. The binary provider owns their use.
  bool NormalNonfaultingExecution = false;
  bool X64CetDisabled = false;
  /// Demand-driven precision refinement for unresolved control. Discovered
  /// register/frame ranges retain exhaustive finite relations. Repeatedly
  /// unresolved memory dependencies may additionally separate proven constant
  /// contexts, under the same global bounds; no input value is assumed.
  bool DiscoverControlState = false;
  /// Explicit context-key ranges, also included in bounded joint relations.
  /// Discovery may add further ranges. Unknown is valid, not an assumed zero.
  /// Other constant bytes join by intersection, so changing business values
  /// do not unroll a loop forever.
  std::vector<symbolic::SymRegisterRange> ControlRegisters;
  /// Optional entry-relative frame identity. This enables affine pointers,
  /// complete affine pointer spills, and constant frame-byte propagation,
  /// not a private/non-aliasing memory claim.
  /// The initial implementation requires an eight-byte register root.
  std::optional<symbolic::SymRegisterRange> FrameBaseRegister;
  /// Context hints only: missing bytes remain unknown. Other frame facts join
  /// by intersection, so changing spilled business values do not unroll loops.
  std::vector<SpecializationFrameSlot> ControlFrameSlots;
  /// An outer RETURN requires the original frame-base value and no writes to
  /// its entry [0, 8) control slot. With a provider native-stack certificate,
  /// non-entry RETURN instead loads a proved exact destination and advances
  /// the stack. Uncertified return dispatch remains unsupported.
  bool RequireRestoredFrameAtReturn = false;
  /// Explicit source-ABI precondition: every external-origin store target
  /// range, including computed external addresses, is disjoint from the entry
  /// control slot. Frame-derived or unknown-origin addresses are never
  /// exempted by this precondition.
  bool ExternalStoresPreserveEntryReturnSlot = false;
  /// Physical-register preconditions supplied by the caller, not values
  /// inferred from a run. Instruction-local lifter temporaries are not ABI
  /// inputs and cannot be bound here.
  std::vector<SpecializationConstant> EntryConstants;
  llvm::endianness ByteOrder = llvm::endianness::little;
  uint32_t MaxNodes = 4096;
  uint32_t MaxContextsPerAddress = 64;
  uint64_t MaxOperations = 262144;
  uint32_t MaxNodeEvaluations = 16384;
  uint32_t MaxIndirectTargets = 16;
  /// Bounds distinct native return slots retained in one context. Native
  /// returns are physical control transfers, not assumed LIFO function exits.
  uint32_t MaxNativeReturnSlots = 64;
  /// Maximum exhaustive value sets used for immutable addresses and joint
  /// control-state projection. A partial solver enumeration is never a fact.
  uint32_t MaxImmutableReadAddresses = 16;
  uint32_t MaxControlTuples = 32;
  uint32_t MaxControlFields = 16;
  /// Refinement restarts the graph from entry. Global node, operation,
  /// evaluation, and solver-query budgets are cumulative across restarts and
  /// backward dependency replay after a failed attempt;
  /// these additionally bound discovery itself. Per-address contexts, active
  /// native return slots, fields, and tuples remain per-attempt structural
  /// limits. Context promotion adds no guest-memory reads: finite-value proof
  /// alone would not establish the accessibility of a new load.
  uint32_t MaxControlRefinements = 16;
  /// Includes dependency DAG visits and observed-predecessor traversal. Replay
  /// carries only candidate bit demands; publication needs a fresh fixed point.
  uint64_t MaxDiscoveryVisits = 65536;
  /// Global check count, per-query encoding/search limits, and per-node DAG
  /// bound. These limits also apply in builds without the optional Z3 backend;
  /// specialization uses the always-available built-in bitvector solver.
  uint64_t MaxSolverQueries = 4096;
  uint64_t MaxSolverGates = 262144;
  uint64_t MaxSolverConflicts = 10000;
  uint64_t MaxSolverPropagations = 1000000;
  uint64_t MaxSolverWatchVisits = 10000000;
  uint64_t MaxSymbolicNodes = 262144;
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
  /// Includes the additional pure operations generated for certified reads
  /// and finite dispatch; fixed-point reevaluations are charged again.
  uint64_t EvaluatedOperations = 0;
  uint32_t NodeEvaluations = 0;
  uint32_t Contexts = 0;
  uint64_t SolverQueries = 0;
  uint32_t RelationalWidenings = 0;
  uint32_t DiscoveredControlFields = 0;
  uint32_t DiscoveredContextFields = 0;
  uint32_t ControlRefinements = 0;
  uint64_t DiscoveryVisits = 0;

  bool complete() const { return Status == SpecializationStatus::Complete; }
};

/// Provider-neutral partial evaluation of strictly lifted integer/control
/// LowIR. Uncertified ordinary memory effects remain in the residual CFG.
/// Exhaustively covered, certified immutable reads may become pure selections.
/// Provider-certified native near CALL/RETURN uses physical stack semantics;
/// ordinary calls without that certificate are refused. Opaque semantics other
/// than retained x64 runtime flag snapshots and restores, ordered memory, and
/// unsupported instruction guards are refused.
/// Every temporary read must have a complete definition in the same lifted
/// native instruction; temporary offsets may be reused by later instructions.
/// Flag snapshots are treated as unknown values for proof and may not certify
/// an external pointer or finite target on their own. A PUSHFQ snapshot needs
/// prior definitions for every modelled flag unless ExplicitMachineState
/// supplies their incoming values. Synthetic labels
/// identify clones; NativeInstruction
/// preserves provenance without copying stale address-occurrence certificates.
SpecializationResult
specializeInterpreter(SpecializationProvider &Provider,
                      SpecializationCursor Entry,
                      const SpecializationOptions &Options = {});

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_INTERPRETERSPECIALIZATION_H
