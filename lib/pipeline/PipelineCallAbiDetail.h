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
#include "neverd/loader/DataSymbolBinding.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"

#include "llvm/ADT/ArrayRef.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;
struct AnalysisCacheWarmup;
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
  /// Every callee summary, user prototype and name a run of the module
  /// recovery holds under one key, as a forwarder probe reads them
  /// (propagateForwardedCallArities).
  struct ProbeRead {
    va_t Key = 0;
    std::optional<std::string> Name;
    std::optional<int> RegArity;
    std::optional<int> TotalArity;
    std::optional<int> FPArity;
    std::optional<uint16_t> FPReturnSize;
    std::optional<std::vector<uint64_t>> FPRegs;
    std::optional<bool> HasSret;
    std::optional<bool> IsVariadic;
    std::optional<bool> ConsumesVaList;
    std::optional<std::vector<uint64_t>> IntRegs;
    bool VectorArguments = false;
    bool UserPrototype = false;
    bool operator==(const ProbeRead &) const = default;
  };
  /// What a forwarder probe of a body gives its callers: the recovered
  /// parameters as they count them, and the summaries the recovery leaves
  /// at the body's entry.
  struct ProbeSignature {
    int RegisterArity = 0;
    int MaxArgumentIndex = -1;
    int RecoveryTotalArity = 0;
    bool IsVariadic = false;
    std::vector<uint64_t> FPRegs;
    std::vector<uint64_t> IntegerArgumentRegisters;
    std::optional<int> EntryRegArity;
    std::optional<int> EntryTotalArity;
    std::optional<bool> EntryIsVariadic;
  };
  /// A forwarder probe of a conversion's body: what it read under each key
  /// it read (recoverCallAbi's SummaryKeysRead), the body's entry first,
  /// and what it gave.  A probe that would read the same gives the same.
  struct Probe {
    std::vector<ProbeRead> Reads;
    ProbeSignature Gives;
    /// What the probe counts against the memo's bound.
    size_t Bytes = 0;
  };
  /// What a body's own parameters say of the calls to it before recovery
  /// finds the ones it forwards (recoverModuleCallAbi): a fact of the body
  /// and the image.
  struct CalleeSummary {
    std::vector<uint64_t> IntegerArgumentRegisters;
    int RegArity = 0;
    int TotalArity = 0;
    bool HasSret = false;
    bool IsVariadic = false;
    bool VectorArguments = false;
    bool ConsumesVaList = false;
    int FPArity = 0;
    std::vector<uint64_t> FPRegs;
    uint16_t FPReturnSize = 0;
  };
  /// A conversion of a callee's lift.
  struct Conversion {
    /// The typed body, or null where the conversion fails or the body is too
    /// large to be a signature.
    std::shared_ptr<const MedFunc> Body;
    /// The call summaries the conversion read, under each key it read them
    /// under and the callee's own entry.
    std::vector<SummaryRead> Reads;
    /// What Body and Probes count against the memo's bound.
    size_t Bytes = 0;
    /// The forwarder probes of Body under different summaries, the one used
    /// last first; at most MaxProbes.
    std::vector<Probe> Probes;
    /// What Body says of the calls to it, and the callees it calls: their
    /// entries in the order a run reads them, and the targets of its direct
    /// calls, one for each call.  Read where the memo keeps the body.
    std::optional<CalleeSummary> Summary;
    std::vector<va_t> CalleesRead;
    std::vector<va_t> DirectCallTargets;
    /// Whether \p Result's summaries read as they did for the conversion.
    bool readsHold(const PipelineResult &Result) const;
  };
  /// The probes kept of each conversion's body.  The selections around a
  /// forwarder hold different summaries of its callees.
  static constexpr size_t MaxProbes = 4;
  /// The conversions kept of each callee.  The selections around a callee
  /// call different sets of the imports it calls, whose summaries its
  /// conversion reads.
  static constexpr size_t MaxConversions = 4;
  struct Callee {
    /// Whether the callee was lifted: BlockOps is then known.
    bool Lifted = false;
    /// The operations of each block of the lift, when the lift is complete
    /// and has no unsafe indirect branch; none otherwise.
    std::optional<std::vector<size_t>> BlockOps;
    /// The conversions read under different summaries, the one used last
    /// first; at most MaxConversions.
    std::vector<Conversion> Conversions;
    /// When the memo last handed the callee out (find).
    uint64_t LastUsed = 0;
    /// The conversion \p Result's summaries hold for, now the one used last;
    /// null where none does.
    const Conversion *conversionFor(const PipelineResult &Result);
  };
  /// The bodies the memo holds at most, counted by their blocks and
  /// operations (keep): bodies of a large image's every callee would hold
  /// most of its MedIR.
  static constexpr size_t MaxBytes = size_t{256} << 20;
  /// Read of \p Img with \p Result's function-pointer variables; starts
  /// again when this was read of another image or with other variables.
  void adopt(const BinaryImage &Img, const PipelineResult &Result);
  /// The conversion whose body \p Body is, where the memo holds it.
  Conversion *conversionOf(const MedFunc &Body);
  /// Keeps \p Kept as \p Holder's probe used last, where its probe used
  /// longest ago makes room past MaxProbes.
  void keepProbe(Conversion &Holder, Probe Kept);
  /// The reading of the callee at \p Entry, now the one used last.
  Callee *find(va_t Entry);
  /// Keeps \p Converted as \p Holder's conversion used last, where its
  /// conversion used longest ago makes room past MaxConversions.  Past
  /// MaxBytes, the bodies of the callees used longest ago go, \p Holder's
  /// aside, down to three quarters of it.
  const Conversion &keep(Callee &Holder, Conversion Converted);
  /// What the kept bodies count against MaxBytes.
  size_t bytes() const { return Bytes; }
  /// The entries callee lifts know: the image's function symbols that are
  /// not boundary guesses, read when the memo adopts the image.
  const std::set<va_t> &functionEntries() const { return FunctionEntries; }
  /// The function-pointer variables of the image the memo adopted
  /// (collectPointerVariableCallees), read when it adopts the image.
  const std::map<va_t, PointerVariableCallee> &pointerCallees() const {
    return PointerCallees;
  }
  /// The code owners of the image the memo adopted, indexed for the lifts of
  /// the callees read of it, where they first need them.  Like the analysis
  /// cache's, the index serves until the memo adopts another image or the
  /// cache is reset.
  const ExecutableCodeOwnerIndex &codeOwners();

  std::map<va_t, Callee> Callees;

private:
  const BinaryImage *Image = nullptr;
  const Symbol *SymbolData = nullptr;
  size_t SymbolCount = 0;
  std::map<va_t, va_t> CallArgumentSources;
  std::set<va_t> FunctionEntries;
  std::map<va_t, PointerVariableCallee> PointerCallees;
  std::optional<ExecutableCodeOwnerIndex> CodeOwners;
  size_t Bytes = 0;
  uint64_t Clock = 0;
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

/// Read the signature bodies of \p Functions into
/// \p Summaries.CalleeSignatureMemo as a restricted run under \p Summaries'
/// call summaries reads them as omitted callees (recoverModuleCallAbi):
/// every lift, and the conversions of the small ones.  Stops, full, where
/// the memo holds a quarter of its bound.
AnalysisCacheWarmup warmOmittedCallees(const BinaryImage &Img,
                                       const PipelineResult &Summaries,
                                       llvm::ArrayRef<va_t> Functions);

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
