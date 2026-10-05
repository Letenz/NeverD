//===- LLVMScalarStateProjection.h - Declared state closure -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_LLVMSCALARSTATEPROJECTION_H
#define NEVERD_ANALYSIS_LLVMSCALARSTATEPROJECTION_H

#include "neverd/analysis/LLVMScalarResultProjection.h"

namespace neverd::analysis {

struct LLVMScalarStateEntryMask {
  unsigned Cell = 0;
  uint64_t And = 0, Or = 0;
};

struct LLVMScalarStateObservation {
  unsigned Offset = 0;
  unsigned Bits = 64;
  /// Observe final XOR effective entry value for this exact byte window.
  bool DifferenceFromEntry = false;
};

struct LLVMScalarStateContract {
  /// Accessible, initialized, defined state bytes, aligned to eight bytes and
  /// disjoint from other storage. The source must access only this object.
  unsigned StateBytes = 0;
  /// All cells are retained as noundef arguments in increasing byte order.
  unsigned CellBits = 32;
  /// Explicit domain parameterization: cell -> (input & And) | Or. At most
  /// one mask per cell; unspecified cells retain every input bit. These are
  /// caller assumptions, never inferred entry validity or ABI facts.
  std::vector<LLVMScalarStateEntryMask> Entry;
  /// Each observation becomes a field after the original status field.
  /// Omitting a state write is the caller's explicit observer contract.
  std::vector<LLVMScalarStateObservation> Observations;
};

struct LLVMScalarStateProjectionLimits {
  uint64_t MaxConstructionWork = 1048576;
  unsigned MaxStateBytes = 4096;
  unsigned MaxArguments = 4096;
  unsigned MaxObservations = 256;
  /// Applied separately to original state admission and result admission.
  LLVMInterpreterModelLimits Model;
};

struct LLVMScalarStateProjectionResult {
  enum Kind { Projected, Unsupported, BudgetExceeded } Status = Unsupported;
  std::string Diagnostic;
  /// One definition with the original name, plus required intrinsics. It
  /// borrows the input context, not the input module. Return field zero is
  /// the original i64 status; subsequent fields follow Observations exactly.
  std::unique_ptr<llvm::Module> Module;
  uint64_t ConstructionWork = 0;
  uint64_t Loads = 0, Stores = 0, Arguments = 0;
};

/// Close a fixed state object over scalar inputs and declared observations.
/// The shared state importer owns source admission, fixed pointer projections,
/// alignment, initialization and operation contracts. Guest/external accesses
/// and address-valued state-pointer uses are refused. Its current layout is
/// little-endian, integral, 64-bit pointers and indices; target names supply
/// no additional authority.
///
/// Every scalar instruction and source obligation remains, even for omitted
/// outputs. Only state memory and its pointer projections become private SSA
/// input-width cells; partial updates preserve other bits through masks.
/// Promotion does not run DCE or arithmetic optimization. Return
/// range attributes become checked assume conditions using the importer's
/// shared range recipe; they never restrict the proof's input domain.
/// Construction limits are cumulative; LLVM cloning and mem2reg are trusted
/// bulk operations without hard CPU/stack bounds. Refusal publishes no module
/// and leaves the original function and its module unchanged.
///
/// Success is a conditional projection, not a proof of defined termination,
/// entry validity, observation sufficiency, native ABI or binary equivalence.
/// Use projectLLVMScalarResult and complete scalar proofs for every required
/// observation before shortening the interface or publishing recovered code.
LLVMScalarStateProjectionResult
projectLLVMScalarState(const llvm::Function &Function,
                       const LLVMScalarStateContract &Contract,
                       const LLVMScalarStateProjectionLimits &Limits = {});

} // namespace neverd::analysis
#endif
