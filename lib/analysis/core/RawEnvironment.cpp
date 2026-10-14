//===- RawEnvironment.cpp - Raw recovery integrity ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "RawEnvironment.h"

#include "../arch/x86_64/X64Recovery.h"

#include "neverd/ir/low/LowRawEnvironment.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/SHA256.h"

#include <bit>

namespace neverd::analysis {
namespace {
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
}

// Versioned, length-prefixed bytes and little-endian numbers. Never hash C++
// object representation, pointer values, padding or host byte order.
class Digest {
  llvm::SHA256 Hash;

public:
  explicit Digest(llvm::StringRef Version) { text(Version); }
  void number(uint64_t Value) {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Value);
    Hash.update(llvm::ArrayRef<uint8_t>(Bytes));
  }
  void bytes(llvm::ArrayRef<uint8_t> Bytes) {
    number(Bytes.size());
    Hash.update(Bytes);
  }
  void text(llvm::StringRef Text) {
    number(Text.size());
    Hash.update(Text);
  }
  void variable(const NdVar &V) {
    number(static_cast<unsigned>(V.Space));
    number(V.Offset);
    number(V.Size);
    number(static_cast<unsigned>(V.Provenance));
    number(V.AddressOwnerVA);
  }
  void boundary(const LowInstructionBoundary &B) {
    number(B.Address);
    number(B.Size);
    number(B.FirstOp);
    number(B.OpCount);
    number(static_cast<unsigned>(B.Mode));
    number(static_cast<unsigned>(B.Control));
    number(static_cast<unsigned>(B.ControlFlags));
    number(static_cast<unsigned>(B.TargetMode));
    number(B.Immediate.has_value());
    if (B.Immediate)
      number(*B.Immediate);
  }
  void operation(const LowOp &Op) {
    number(static_cast<unsigned>(Op.Opcode));
    number(Op.Addr);
    number(Op.Seq);
    variable(Op.Output);
    number(Op.NumInputs);
    for (const auto &V : Op.Inputs)
      variable(V);
    number(static_cast<unsigned>(Op.MemoryOrdering));
    number(static_cast<unsigned>(Op.MemoryAddressSpace));
  }
  template <typename Range> void numbers(const Range &Values) {
    number(Values.size());
    for (const auto V : Values)
      number(V);
  }
  std::string finish() { return llvm::toHex(Hash.final()); }
};

bool validDigest(llvm::StringRef Text) {
  return Text.size() == 64 && llvm::all_of(Text, [](char C) {
           return (C >= '0' && C <= '9') || (C >= 'A' && C <= 'F');
         });
}

std::string optionsDigest(const SpecializationOptions &O) {
  Digest H("neverd-raw-environment-options-v1");
  H.number(O.ExplicitMachineState);
  H.number(O.NormalNonfaultingExecution);
  H.number(O.X64CetDisabled);
  H.number(O.X64SegmentedReads);
  H.number(O.X64CR8State);
  H.number(O.X64TerminalImports.size());
  for (const auto &B : O.X64TerminalImports) {
    H.number(B.Version);
    H.number(B.Slot);
    H.text(B.Module);
    H.text(B.Symbol);
  }
  H.number(O.X64CodeRegions.size());
  for (const auto &R : O.X64CodeRegions) {
    H.number(R.Version);
    H.number(R.Address);
    H.bytes(R.Bytes);
    H.text(R.Evidence);
  }
  H.number(O.X64ImportBindings.size());
  for (const auto &B : O.X64ImportBindings) {
    H.number(B.Version);
    H.number(B.Slot);
    H.text(B.Module);
    H.text(B.Symbol);
    H.number(B.Target);
    H.text(B.Evidence);
  }
  H.number(O.X64FlagsProfile.has_value());
  if (O.X64FlagsProfile)
    H.number(static_cast<unsigned>(*O.X64FlagsProfile));
  H.number(O.DiscoverControlState);
  H.number(O.ControlRegisters.size());
  for (const auto &R : O.ControlRegisters) {
    H.number(R.Offset);
    H.number(R.Bytes);
  }
  H.number(O.FrameBaseRegister.has_value());
  if (O.FrameBaseRegister) {
    H.number(O.FrameBaseRegister->Offset);
    H.number(O.FrameBaseRegister->Bytes);
  }
  H.number(O.EntryFrameBounds.has_value());
  if (O.EntryFrameBounds) {
    H.number(O.EntryFrameBounds->Begin);
    H.number(O.EntryFrameBounds->End);
  }
  H.number(O.EntryFrameAlignment.has_value());
  if (O.EntryFrameAlignment) {
    H.number(O.EntryFrameAlignment->Alignment);
    H.number(O.EntryFrameAlignment->Residue);
  }
  H.number(O.ControlFrameSlots.size());
  for (const auto &R : O.ControlFrameSlots) {
    H.number(R.Offset);
    H.number(R.Bytes);
  }
  H.number(O.RequireRestoredFrameAtReturn);
  H.number(O.ProveStoreReturnSlotSeparation);
  H.number(O.ExternalStoresPreserveEntryReturnSlot);
  H.number(O.ExternalStoresDisjointEntryFrame);
  H.number(O.EntryConstants.size());
  for (const auto &C : O.EntryConstants) {
    H.variable(C.Location);
    H.number(C.Value);
  }
  H.number(static_cast<unsigned>(O.ByteOrder));
  H.number(O.MaxImagePreparationBytes);
  H.number(O.MaxImagePreparationRecords);
  H.number(O.MaxRawEnvironmentResultBytes);
  H.number(O.MaxRawEnvironmentResultRecords);
  H.number(O.MaxNodes);
  H.number(O.MaxContextsPerAddress);
  H.number(O.MaxOperations);
  H.number(O.MaxNodeEvaluations);
  H.number(O.MaxIndirectTargets);
  H.number(O.MaxChainedTransfers);
  H.number(O.StopChainingAtRepeatedDestination);
  H.number(O.MaxNativeReturnSlots);
  H.number(O.MaxImmutableReadAddresses);
  H.number(O.MaxControlTuples);
  H.number(O.MaxControlFields);
  H.number(O.MaxControlRefinements);
  H.number(O.MaxDiscoveryVisits);
  H.number(O.MaxSolverQueries);
  H.number(O.MaxSolverGates);
  H.number(O.MaxSolverConflicts);
  H.number(O.MaxSolverPropagations);
  H.number(O.MaxSolverWatchVisits);
  H.number(O.MaxSymbolicNodes);
  H.number(O.MaxChainedVisitsPerDestination);
  H.number(O.MaxFrameOffsetNormalizationWorkPerProof);
  return H.finish();
}

std::string domainDigest(const SpecializationRawEnvironment &D,
                         llvm::StringRef OptionsDigest) {
  Digest H("neverd-raw-environment-domain-v1");
  H.number(D.Version);
  H.text(OptionsDigest);
  H.number(D.PrimaryImageBegin);
  H.number(D.PrimaryImageEnd);
  H.text(D.PrimaryImageDigest);
  return H.finish();
}

llvm::Error validatePrimaryExtent(const SpecializationRawEnvironment &D) {
  if (D.Version != 1 || !D.Declaration.hasRawEnvironment() ||
      !validDigest(D.PrimaryImageDigest) ||
      D.PrimaryImageBegin >= D.PrimaryImageEnd)
    return invalid("invalid raw environment primary image evidence");
  for (const auto &R : D.Declaration.X64CodeRegions)
    if (R.Address < D.PrimaryImageEnd &&
        D.PrimaryImageBegin < R.Address + R.Bytes.size())
      return invalid("supplemental code overlaps the primary image extent");
  for (const auto &B : D.Declaration.X64ImportBindings)
    if (B.Slot < D.PrimaryImageBegin || B.Slot >= D.PrimaryImageEnd ||
        8 > D.PrimaryImageEnd - B.Slot)
      return invalid("stable import slot is outside the primary image extent");
  return llvm::Error::success();
}

llvm::Error preflightResult(const SpecializationResult &R,
                            const SpecializationOptions &Expected) {
  // Independent expected limits bound all later graph/receipt walks, including
  // the older external-exit validator. Stored limits are never consulted.
  uint64_t Bytes = Expected.MaxRawEnvironmentResultBytes / 8;
  uint64_t Records = Expected.MaxRawEnvironmentResultRecords / 8;
  const auto AddBytes = [&](uint64_t N) {
    if (N > Bytes)
      return false;
    Bytes -= N;
    return true;
  };
  const auto AddRecords = [&](uint64_t N, uint64_t Width) {
    if (N > Records || N > Bytes / Width)
      return false;
    Records -= N;
    Bytes -= N * Width;
    return true;
  };
  const auto Exhausted = [] {
    return llvm::createStringError(
        std::make_error_code(std::errc::value_too_large),
        "raw environment result validation budget exhausted");
  };
  const auto &F = R.Residual;
  if (!AddBytes(2048) || !AddBytes(F.Name.size()) ||
      !AddBytes(F.DebugName.size()) || !AddBytes(F.SourceFile.size()) ||
      !AddBytes(R.Diagnostic.size()) || !AddRecords(F.Blocks.size(), 128) ||
      !AddRecords(F.ModuleAnalysisRoots.size(), 8) ||
      !AddRecords(F.OrdinaryModuleAnalysisRoots.size(), 8) ||
      !AddRecords(F.FunctionTemporaries.size(), 32) ||
      !AddRecords(R.Origins.size(), 160) || !AddRecords(R.Reads.size(), 128) ||
      !AddRecords(R.ExternalExits.size(), 2048))
    return Exhausted();
  // The existing exit validator builds ordered origin/exit indexes and
  // compares each exit with the declaration list. Pay those variable terms
  // explicitly, before it allocates indexes, walks names or hashes evidence.
  const auto IndexWork = [&](uint64_t Count) {
    const uint64_t Comparisons = 2 * std::bit_width(Count) + 2;
    return Count <= Records / Comparisons && AddRecords(Count * Comparisons, 8);
  };
  if (!IndexWork(R.Origins.size()) || !IndexWork(R.ExternalExits.size()))
    return Exhausted();
  const uint64_t Exits = R.ExternalExits.size();
  const uint64_t Terminals = Expected.X64TerminalImports.size();
  const uint64_t OriginLookups = std::bit_width(uint64_t(R.Origins.size())) + 1;
  if (Exits > Records / OriginLookups ||
      !AddRecords(Exits * OriginLookups, 8) ||
      (Exits && Terminals > Records / Exits) ||
      !AddRecords(Exits * Terminals, 32))
    return Exhausted();
  if (Exits)
    for (const auto &Binding : Expected.X64TerminalImports)
      for (size_t Length : {Binding.Module.size(), Binding.Symbol.size()})
        if (Length > Bytes / Exits || !AddBytes(Length * Exits))
          return Exhausted();
  const uint64_t ExitLookups = std::bit_width(Exits) + 1;
  for (const auto &B : F.Blocks)
    if (!AddRecords(B.Ops.size(), 512) ||
        !AddRecords(B.InstructionBoundaries.size(), 128) ||
        B.InstructionBoundaries.size() > Records / ExitLookups ||
        !AddRecords(B.InstructionBoundaries.size() * ExitLookups, 8) ||
        !AddRecords(B.Preds.size(), 8) || !AddRecords(B.Succs.size(), 8))
      return Exhausted();
  for (const auto &Read : R.Reads)
    if (!AddBytes(Read.Bytes.size()) || !AddBytes(Read.Evidence.size()))
      return Exhausted();
  for (const auto &E : R.ExternalExits) {
    const auto &I = E.Original;
    if (!AddRecords(I.Ops.size(), 512) || !AddBytes(I.NativeBytes.size()) ||
        !AddRecords(I.UndefinedEffects.Effects.size(), 256) ||
        !AddBytes(I.UndefinedEffects.OperationDigest.size()) ||
        !AddBytes(I.UndefinedEffects.Diagnostic.size()) ||
        !AddBytes(I.PreservedState.NativeBytesDigest.size()) ||
        !AddBytes(I.PreservedState.OperationDigest.size()) ||
        !AddBytes(E.IdentityDigest.size()) ||
        !AddBytes(E.OriginalOperationDigest.size()) ||
        !AddBytes(E.ExpandedOperationDigest.size()) ||
        !AddBytes(E.ResidualOperationDigest.size()))
      return Exhausted();
    if (I.TerminalImport &&
        (!AddBytes(I.TerminalImport->Binding.Module.size()) ||
         !AddBytes(I.TerminalImport->Binding.Symbol.size()) ||
         !AddBytes(I.TerminalImport->FixedImageDigest.size())))
      return Exhausted();
    // Validate before legacy helpers index the fixed operand storage.
    for (const auto &Op : I.Ops)
      if (Op.NumInputs > std::size(Op.Inputs))
        return invalid("raw environment exit has an invalid operand count");
  }
  return llvm::Error::success();
}

llvm::Expected<std::string> graphDigest(const LowFunc &F) {
  // The raw publisher creates ordinary blocks and explicit roots, not stale
  // loader/EH/address-occurrence certificates. Refuse unfamiliar sidecars
  // instead of silently leaving them outside the integrity boundary.
  if (!F.JumpTables.empty() || !F.CxxContinuationExits.empty() ||
      !F.CxxContinuationEntries.empty() ||
      !F.SEHLocalUnwindContinuations.empty() ||
      F.CxxContinuationExitAnalysisComplete ||
      !F.RelocatedInstructionAddressOccurrences.empty() ||
      !F.I386GetPcOccurrences.empty() ||
      !F.RelocatedInstructionScalarModelOccurrences.empty() ||
      !F.EverPublishedJumpTableBranchAddresses.empty() ||
      !F.UnsafeIndirectBranchAddresses.empty() || F.ExceptionMetadata ||
      F.RegistrationStates || !F.DecodeFailureAddresses.empty() ||
      !F.UnsupportedInstructionAddresses.empty() ||
      !F.UnprovenReturnAddresses.empty() || !F.TruncatedPathAddresses.empty() ||
      F.CalleePopBytes || !F.CodeRefTargets.empty())
    return invalid("raw environment graph carries unsupported sidecars");
  Digest H("neverd-raw-environment-graph-v1");
  H.number(F.Entry);
  H.number(F.OriginalSize);
  H.text(F.Name);
  H.text(F.DebugName);
  H.text(F.SourceFile);
  H.number(F.SourceLine);
  H.number(F.DecodedInstructionCount);
  H.number(F.LiftedInstructionCount);
  H.numbers(F.ModuleAnalysisRoots);
  H.numbers(F.OrdinaryModuleAnalysisRoots);
  H.number(F.FunctionTemporaries.size());
  for (const auto &T : F.FunctionTemporaries) {
    H.number(T.Offset);
    H.number(T.Bytes);
  }
  H.number(F.Blocks.size());
  for (const auto &B : F.Blocks) {
    if (!B.ExceptionalSuccs.empty() || !B.ExceptionalPreds.empty())
      return invalid("raw environment graph carries exceptional edges");
    H.number(B.Id);
    H.number(B.StartAddr);
    H.number(B.EndAddr);
    H.numbers(B.Preds);
    H.numbers(B.Succs);
    H.number(B.Ops.size());
    for (const auto &Op : B.Ops) {
      if (Op.NumInputs > std::size(Op.Inputs))
        return invalid("raw environment graph has an invalid operand count");
      H.operation(Op);
    }
    H.number(B.InstructionBoundaries.size());
    for (const auto &Boundary : B.InstructionBoundaries)
      H.boundary(Boundary);
  }
  if (auto Error = validateLowInstructionBoundaries(
          F, LowInstructionBoundaryRequirement::Required))
    return std::move(Error);
  return H.finish();
}

std::string evidenceDigest(const SpecializationResult &R) {
  Digest H("neverd-raw-environment-evidence-v1");
  H.number(R.Origins.size());
  for (const auto &O : R.Origins) {
    H.number(O.ResidualAddress);
    H.boundary(O.NativeInstruction);
  }
  H.number(R.Reads.size());
  for (const auto &Read : R.Reads) {
    H.number(Read.InstructionAddress);
    H.number(Read.OpSeq);
    H.number(Read.Address);
    H.bytes(Read.Bytes);
    H.text(Read.Evidence);
  }
  H.number(R.ExternalExits.size());
  for (const auto &E : R.ExternalExits) {
    // The external-exit validator independently recomputes this identity from
    // the complete original instruction, physical expansion and occurrence.
    H.text(E.IdentityDigest);
    H.text(E.Original.UndefinedEffects.Diagnostic);
    // Also bind inactive fixed operand slots, which the older semantic
    // operation digest deliberately ignores. Raw receipt integrity is exact.
    H.number(E.Original.Ops.size());
    for (const auto &Op : E.Original.Ops)
      H.operation(Op);
  }
  H.number(static_cast<unsigned>(R.Status));
  H.text(R.Diagnostic);
  H.number(R.EvaluatedOperations);
  H.number(R.NodeEvaluations);
  H.number(R.Contexts);
  H.number(R.SolverQueries);
  H.number(R.RelationalWidenings);
  H.number(R.DiscoveredControlFields);
  H.number(R.DiscoveredContextFields);
  H.number(R.ControlRefinements);
  H.number(R.DiscoveryVisits);
  return H.finish();
}

std::string receiptDigest(const SpecializationRawEnvironmentReceipt &R) {
  Digest H("neverd-raw-environment-receipt-v1");
  H.number(R.Version);
  H.text(R.Domain->DomainDigest);
  H.text(R.GraphDigest);
  H.text(R.EvidenceDigest);
  return H.finish();
}
} // namespace

llvm::Expected<std::shared_ptr<const SpecializationRawEnvironment>>
prepareRawEnvironment(const SpecializationOptions &Options, va_t PrimaryBegin,
                      va_t PrimaryEnd, llvm::StringRef PrimaryDigest) {
  auto Cost = x64::validateRawEnvironmentDeclarations(Options);
  if (!Cost)
    return Cost.takeError();
  auto D = std::make_shared<SpecializationRawEnvironment>();
  D->Declaration = Options;
  D->PrimaryImageBegin = PrimaryBegin;
  D->PrimaryImageEnd = PrimaryEnd;
  D->PrimaryImageDigest = PrimaryDigest;
  if (auto Error = validatePrimaryExtent(*D))
    return std::move(Error);
  D->DomainDigest = domainDigest(*D, optionsDigest(Options));
  return D;
}

llvm::Error validatePreparedRawEnvironment(
    const std::shared_ptr<const SpecializationRawEnvironment> &Domain,
    const SpecializationOptions &Options) {
  if (!Options.hasRawEnvironment())
    return Domain ? invalid("unexpected prepared raw environment")
                  : llvm::Error::success();
  auto ExpectedCost = x64::validateRawEnvironmentDeclarations(Options);
  if (!ExpectedCost)
    return ExpectedCost.takeError();
  if (!Domain)
    return invalid("raw environment declarations require a prepared provider");
  // Never let a stored declaration authorize its own larger traversal budget.
  // Compare these scalars before examining any stored collection or payload.
  if (Domain->Declaration.MaxImagePreparationBytes !=
          Options.MaxImagePreparationBytes ||
      Domain->Declaration.MaxImagePreparationRecords !=
          Options.MaxImagePreparationRecords ||
      Domain->Declaration.MaxRawEnvironmentResultBytes !=
          Options.MaxRawEnvironmentResultBytes ||
      Domain->Declaration.MaxRawEnvironmentResultRecords !=
          Options.MaxRawEnvironmentResultRecords)
    return invalid("raw environment differs from its expected budgets");
  auto StoredCost =
      x64::validateRawEnvironmentDeclarations(Domain->Declaration);
  if (!StoredCost)
    return StoredCost.takeError();
  if (auto Error = validatePrimaryExtent(*Domain))
    return Error;
  const auto Expected = optionsDigest(Options);
  const auto Stored = optionsDigest(Domain->Declaration);
  if (Expected != Stored ||
      Domain->DomainDigest != domainDigest(*Domain, Stored))
    return invalid("raw environment differs from its expected declaration");
  return llvm::Error::success();
}

std::array<uint64_t, 4>
rawEnvironmentDigestWords(const SpecializationRawEnvironment &Domain) {
  assert(validDigest(Domain.DomainDigest));
  const auto Bytes = llvm::fromHex(Domain.DomainDigest);
  std::array<uint64_t, 4> Words;
  for (size_t I = 0; I != Words.size(); ++I)
    Words[I] = llvm::support::endian::read64le(Bytes.data() + I * 8);
  return Words;
}

llvm::Error
validateSpecializationRawEnvironment(const SpecializationResult &Result,
                                     const SpecializationOptions &Options) {
  if (!Options.hasRawEnvironment()) {
    if (Result.RawEnvironment ||
        hasLowRawEnvironmentRequirement(Result.Residual))
      return invalid("raw environment evidence lacks its expected declaration");
    return llvm::Error::success();
  }
  if (!Result.complete() || !Result.RawEnvironment ||
      Result.RawEnvironment->Version != 1)
    return invalid("raw environment requires a complete versioned receipt");
  if (auto Error = preflightResult(Result, Options))
    return Error;
  const auto &Receipt = *Result.RawEnvironment;
  if (auto Error = validatePreparedRawEnvironment(Receipt.Domain, Options))
    return Error;
  const auto &F = Result.Residual;
  if (F.Blocks.empty() || F.Blocks.front().Id != 0 ||
      F.Blocks.front().StartAddr != F.Entry ||
      F.Blocks.front().EndAddr != F.Entry + 1 ||
      !F.Blocks.front().Preds.empty() || F.Blocks.front().Ops.size() != 2 ||
      F.Blocks.front().Ops.back().Opcode != NdOp::BRANCH)
    return invalid("raw environment requires its original entry anchor");
  const auto &Marker = F.Blocks.front().Ops.front();
  if (!lowRawEnvironmentRequirementShapeIsValid(Marker) ||
      Marker.Addr != F.Entry || Marker.Seq != 0)
    return invalid("raw environment entry requirement is missing or malformed");
  const auto Words = rawEnvironmentDigestWords(*Receipt.Domain);
  for (size_t I = 0; I != Words.size(); ++I)
    if (Marker.Inputs[I + 1].Offset != Words[I])
      return invalid("raw environment entry requirement has a stale digest");
  size_t Occurrences = 0;
  for (const auto &B : F.Blocks)
    for (const auto &Op : B.Ops)
      Occurrences += isLowRawEnvironmentRequirement(Op);
  if (Occurrences != 1)
    return invalid("raw environment requirement must occur exactly once");
  auto Graph = graphDigest(F);
  if (!Graph)
    return Graph.takeError();
  if (*Graph != Receipt.GraphDigest)
    return invalid("raw environment graph differs from its receipt");
  if (auto Error = validateSpecializationExternalExits(Result, Options))
    return Error;
  if (Receipt.EvidenceDigest != evidenceDigest(Result) ||
      Receipt.IdentityDigest != receiptDigest(Receipt))
    return invalid("raw environment evidence differs from its receipt");
  return llvm::Error::success();
}

llvm::Error sealRawEnvironment(
    SpecializationResult &Result,
    const std::shared_ptr<const SpecializationRawEnvironment> &Domain,
    const SpecializationOptions &Options) {
  if (auto Error = preflightResult(Result, Options))
    return Error;
  auto Graph = graphDigest(Result.Residual);
  if (!Graph)
    return Graph.takeError();
  SpecializationRawEnvironmentReceipt Receipt;
  Receipt.Domain = Domain;
  Receipt.GraphDigest = std::move(*Graph);
  Receipt.EvidenceDigest = evidenceDigest(Result);
  Receipt.IdentityDigest = receiptDigest(Receipt);
  Result.RawEnvironment = std::move(Receipt);
  return validateSpecializationRawEnvironment(Result, Options);
}
} // namespace neverd::analysis
