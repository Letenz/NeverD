//===- PipelineCallAbiDetail.h - Module call-ABI recovery -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private declaration of the whole-module call-ABI recovery that the
/// patch/lift and HighIR pipeline drivers share.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINECALLABIDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINECALLABIDETAIL_H

#include "neverd/Common.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/med/MedIR.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;
struct PipelineResult;
struct Symbol;

/// What reading the signature bodies of omitted callees read of each callee
/// (recoverModuleCallAbi): how its lift's blocks count against the bound on
/// the operations read, and the typed body its conversion gives.  The lift
/// follows from the image; the conversion also from the call summaries it
/// read, so a restricted run takes it as read where those summaries read the
/// same under the keys the conversion read them under.
struct OmittedCalleeMemo {
  /// The call summaries of a run under one key.
  struct SummaryRead {
    va_t Key = 0;
    std::optional<uint32_t> MayWrite;
    std::optional<GPRReadWidths> EntryReads;
    std::optional<int> StackArgs;
    bool StackSlot = false;
    std::optional<int> ArgumentCount;
    bool operator==(const SummaryRead &) const = default;
  };
  static SummaryRead readAt(const PipelineResult &Result, va_t Key);
  struct Callee {
    /// Whether the callee was lifted: BlockOps is then known.
    bool Lifted = false;
    /// The operations of each block of the lift, when the lift is complete
    /// and has no unsafe indirect branch; none otherwise.
    std::optional<std::vector<size_t>> BlockOps;
    /// Whether the lift was converted: Body is then known.
    bool Converted = false;
    /// The typed body, or null where the conversion fails or the body is too
    /// large to be a signature.
    std::shared_ptr<const MedFunc> Body;
    /// The call summaries the conversion read, under each key it read them
    /// under and the callee's own entry.
    std::vector<SummaryRead> Reads;
    /// Whether \p Result's summaries read as they did for the conversion.
    bool readsHold(const PipelineResult &Result) const;
  };
  /// Read of \p Img with \p Result's function-pointer variables; starts
  /// again when this was read of another image or with other variables.
  void adopt(const BinaryImage &Img, const PipelineResult &Result);
  /// The callees read, unless the memo is full: the callee is then read for
  /// the run alone.
  Callee *find(va_t Entry);
  /// Counts \p Body against the memo's bound.
  void remember(const MedFunc &Body);

  std::map<va_t, Callee> Callees;

private:
  const BinaryImage *Image = nullptr;
  const Symbol *SymbolData = nullptr;
  size_t SymbolCount = 0;
  std::map<va_t, va_t> CallArgumentSources;
  size_t Bytes = 0;
  bool Full = false;
};

/// Recover every call's arguments in \p Result, and the parameters each
/// forwarder passes straight through, from the setup its caller writes
/// before the call (recoverCallAbi).  Each callee's recovered signature
/// bounds its calls: its integer register and total argument counts, its
/// floating-point argument registers, its scalar floating-point return
/// width, its hidden indirect-result pointer, and whether it is variadic or
/// consumes a va_list.  A forwarder's signature grows from its callee's to a
/// fixed point first, so the order of the functions does not matter.  Reads
/// the types the functions have already been given (inferMedTypes).
void recoverModuleCallAbi(const BinaryImage &Img, PipelineResult &Result,
                          const std::map<va_t, std::string> &AllFuncNames);

/// Give each call in \p Result exactly its callee's parameters, as the LLVM
/// emitter does when it truncates a call to its callee's declaration: once
/// every signature is final, a register the caller set for something else,
/// still live at the call, is no argument of a callee that takes fewer.  (The
/// recovery keeps such a register while a forwarder's parameters may still
/// grow, and the LLVM route's escape checks see it there.)  A variadic callee
/// takes what its caller passes, parameters bound to a source declaration
/// need not count ABI positions, and an import's veneer, lifted like a
/// function, takes the import's arguments.
void matchCallsToCalleeSignatures(const BinaryImage &Img,
                                  PipelineResult &Result);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINECALLABIDETAIL_H
