//===- LLVMPrivateFrameProjection.h - Initialized local memory --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_LLVMPRIVATEFRAMEPROJECTION_H
#define NEVERD_ANALYSIS_LLVMPRIVATEFRAMEPROJECTION_H

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class Argument;
class Function;
class Value;
} // namespace llvm

namespace neverd::analysis {

struct LLVMPrivateFrameObject {
  /// A live AS0 pointer parameter, with at least Bytes accessible bytes.
  /// Its complete extent must be disjoint from the declared frame. Objects
  /// may alias each other; their contents and writes remain observable.
  llvm::Argument *Base = nullptr;
  uint64_t Bytes = 0;
};

struct LLVMPrivateFrameContract {
  /// Fixed numeric entry value: an integer parameter or an entry-block load.
  /// This is supplied by the caller's memory contract, not inferred from a
  /// register number, an LLVM name, a target triple or a known sample offset.
  llvm::Value *Base = nullptr;
  int64_t Begin = 0, End = 0;
  std::vector<LLVMPrivateFrameObject> OtherObjects;
};

struct LLVMPrivateFrameLimits {
  uint64_t MaxInstructions = 65536;
  uint64_t MaxBlocks = 1024;
  uint64_t MaxFrameBytes = 65536;
  /// Total storage for all definite-initialization bit vectors.
  uint64_t MaxDataflowBytes = 8 * 1024 * 1024;
  uint64_t MaxWork = 1048576;
};

struct LLVMPrivateFrameResult {
  enum Kind { Projected, Unsupported, WorkLimitExceeded } Status = Unsupported;
  std::string Diagnostic;
  uint64_t Work = 0, Loads = 0, Stores = 0, Rounds = 0;
  /// Actual referenced byte extent, derived from the current IR.
  int64_t Begin = 0, End = 0;
};

/// Under an explicit caller memory contract, replace initialized numeric
/// frame accesses with a fresh local byte object. Every accessed byte must be
/// live and nonfaulting with its original alignment; [Base+Begin, Base+End)
/// must not wrap, and its final contents must be unobservable. OtherObjects
/// supplies all remaining accessible memory and is disjoint from that frame.
/// These are preconditions, not facts proved by this function.
///
/// The bounded analysis proves complete byte initialization before each read
/// on every CFG predecessor and backedge. It rejects unknown/ordered memory,
/// ordinary calls, exceptional control and unsupported pointer expressions.
/// Only byte-sized integer loads/stores up to 128 bits are admitted. Numeric
/// addresses used as data retain their original values; only memory operands
/// are rewritten. Other object operands become direct constant GEPs without
/// changing their contents. All original scalar instructions remain in place.
///
/// The input must be verified LLVM IR. Refusal leaves it unchanged. Success
/// is a conditional memory projection for defined executions, not proof of
/// source definedness, frame privacy, native ABI, or input independence. No
/// default decompilation pipeline infers this contract or invokes this API.
LLVMPrivateFrameResult
projectLLVMPrivateFrame(llvm::Function &Function,
                        const LLVMPrivateFrameContract &Contract,
                        const LLVMPrivateFrameLimits &Limits = {});

} // namespace neverd::analysis
#endif
