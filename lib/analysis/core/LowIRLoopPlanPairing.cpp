//===- LowIRLoopPlanPairing.cpp - Combine untrusted loop proposals --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LowIRRefinement.h"

#include "llvm/Support/Errc.h"

#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace neverd::analysis {
namespace {

llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

using LocationKey = std::tuple<LowIRLoopSpace, uint64_t, uint16_t>;
LocationKey key(const LowIRLoopLocation &L) {
  return {L.Space, L.Offset, L.Bytes};
}

bool sameAssignments(llvm::ArrayRef<LowIRLoopAssignment> A,
                     llvm::ArrayRef<LowIRLoopAssignment> B) {
  if (A.size() != B.size())
    return false;
  for (size_t I = 0; I != A.size(); ++I)
    if (key(A[I].Location) != key(B[I].Location) || A[I].Value != B[I].Value)
      return false;
  return true;
}

bool sameGuards(llvm::ArrayRef<LowIRLoopGuard> A,
                llvm::ArrayRef<LowIRLoopGuard> B) {
  if (A.size() != B.size())
    return false;
  for (size_t I = 0; I != A.size(); ++I)
    if (key(A[I].Location) != key(B[I].Location) || A[I].Mask != B[I].Mask ||
        A[I].Value != B[I].Value)
      return false;
  return true;
}

struct Renamer {
  std::map<std::pair<uint64_t, uint16_t>, NdVar> Values;
  std::set<uint64_t> Bytes;
  uint64_t &Next;

  llvm::Expected<NdVar> define(NdVar V) {
    if (!V.isTemp() || !V.Size || V.Size > 8 ||
        V.Offset > std::numeric_limits<uint64_t>::max() - (V.Size - 1) ||
        V.Provenance != ConstantAddressProvenance::Unknown ||
        V.AddressOwnerVA != std::numeric_limits<uint64_t>::max())
      return invalid("pairing requires ordinary scalar temporaries");
    for (uint16_t I = 0; I != V.Size; ++I)
      if (!Bytes.insert(V.Offset + I).second)
        return invalid("pairing requires nonoverlapping temporary definitions");
    if (Next > std::numeric_limits<uint64_t>::max() - 8)
      return invalid("pairing temporary namespace exhausted");
    NdVar Renamed = V;
    Renamed.Offset = Next;
    Next += 8;
    Values.emplace(std::make_pair(V.Offset, V.Size), Renamed);
    return Renamed;
  }

  llvm::Expected<NdVar> use(NdVar V) const {
    if (V.isConst())
      return V;
    const auto Found = Values.find({V.Offset, V.Size});
    if (!V.isTemp() || Found == Values.end() ||
        V.Provenance != ConstantAddressProvenance::Unknown ||
        V.AddressOwnerVA != std::numeric_limits<uint64_t>::max())
      return invalid("pairing requires defined exact-width temporary uses");
    return Found->second;
  }
};

llvm::Expected<LowIRLoopCutpoint> pairCut(const LowIRLoopCutpoint &A,
                                          const LowIRLoopCutpoint &B,
                                          const LowIRLoopCutpointPair &Pair) {
  if (A.UseEntryPrefix != B.UseEntryPrefix ||
      ((A.GeneralizeEntryPrefix || B.GeneralizeEntryPrefix) &&
       !A.UseEntryPrefix))
    return invalid("paired cuts have incompatible entry-prefix policies");
  LowIRLoopCutpoint Result;
  Result.OriginalAddress = A.OriginalAddress;
  Result.CandidateAddress = B.OriginalAddress;
  Result.UseEntryPrefix = A.UseEntryPrefix;
  Result.GeneralizeEntryPrefix =
      A.GeneralizeEntryPrefix || B.GeneralizeEntryPrefix;
  Result.OriginalGuards = A.OriginalGuards;
  Result.CandidateGuards = B.OriginalGuards;
  uint64_t Next = 0;
  Renamer Left{{}, {}, Next}, Right{{}, {}, Next};
  std::map<LocationKey, NdVar> LeftInputs, RightInputs;
  std::map<LocationKey, LocationKey> Shared;
  std::set<LocationKey> SharedLeft;
  for (const auto &P : Pair.SharedInputs) {
    if (!P.Original.Bytes || P.Original.Bytes != P.Candidate.Bytes ||
        !Shared.emplace(key(P.Candidate), key(P.Original)).second ||
        !SharedLeft.insert(key(P.Original)).second)
      return invalid("paired inputs must be distinct and have equal widths");
  }
  const auto Inputs = [&](const LowIRLoopCutpoint &Cut, Renamer &Names,
                          bool IsCandidate) -> llvm::Error {
    auto &Locations = IsCandidate ? RightInputs : LeftInputs;
    for (auto I : Cut.Inputs) {
      if (I.Side != LowIRLoopSide::Entry && I.Side != LowIRLoopSide::Original &&
          I.Side != LowIRLoopSide::OriginalPrefix)
        return invalid("pairing requires one-sided self-relation inputs");
      if (I.Side == LowIRLoopSide::OriginalPrefix && !Cut.UseEntryPrefix)
        return invalid("prefix input requires an entry-prefix plan");
      if (I.Temporary.Size != I.Location.Bytes)
        return invalid("loop input width does not match its location");
      auto V = Names.define(I.Temporary);
      if (!V)
        return V.takeError();
      if (I.Side == LowIRLoopSide::Original &&
          !Locations.emplace(key(I.Location), *V).second)
        return invalid("ambiguous induction input location");
      I.Temporary = *V;
      if (IsCandidate) {
        if (I.Side == LowIRLoopSide::Original)
          I.Side = LowIRLoopSide::Candidate;
        else if (I.Side == LowIRLoopSide::OriginalPrefix)
          I.Side = LowIRLoopSide::CandidatePrefix;
      }
      Result.Inputs.push_back(I);
    }
    return llvm::Error::success();
  };
  if (auto Error = Inputs(A, Left, false))
    return std::move(Error);
  if (auto Error = Inputs(B, Right, true))
    return std::move(Error);
  for (const auto &[Candidate, Original] : Shared)
    if (!RightInputs.count(Candidate) || !LeftInputs.count(Original))
      return invalid("paired induction input is absent");
  const auto Expressions = [&](const LowIRLoopCutpoint &Cut,
                               Renamer &Names) -> llvm::Error {
    for (auto O : Cut.Expressions) {
      if (O.NumInputs > std::size(O.Inputs))
        return invalid("invalid loop expression arity");
      for (unsigned I = 0; I != O.NumInputs; ++I) {
        auto V = Names.use(O.Inputs[I]);
        if (!V)
          return V.takeError();
        O.Inputs[I] = *V;
      }
      auto Output = Names.define(O.Output);
      if (!Output)
        return Output.takeError();
      O.Output = *Output;
      Result.Expressions.push_back(O);
    }
    return llvm::Error::success();
  };
  if (auto Error = Expressions(A, Left))
    return std::move(Error);
  if (auto Error = Expressions(B, Right))
    return std::move(Error);
  const auto Assignments =
      [&](const LowIRLoopCutpoint &Cut, Renamer &Names,
          std::vector<LowIRLoopAssignment> &Out) -> llvm::Error {
    for (auto Assignment : Cut.OriginalState) {
      auto V = Names.use(Assignment.Value);
      if (!V)
        return V.takeError();
      Assignment.Value = *V;
      Out.push_back(Assignment);
    }
    return llvm::Error::success();
  };
  if (auto Error = Assignments(A, Left, Result.OriginalState))
    return std::move(Error);
  if (auto Error = Assignments(B, Right, Result.CandidateState))
    return std::move(Error);
  auto AP = Left.use(A.Predicate);
  if (!AP)
    return AP.takeError();
  auto BP = Right.use(B.Predicate);
  if (!BP)
    return BP.takeError();
  if (AP->Size != 1 || BP->Size != 1 || A.Rank.empty() || B.Rank.empty())
    return invalid("pairing requires byte predicates and nonempty ranks");
  const auto Boolean = [&](NdOp Opcode, NdVar L,
                           NdVar R) -> llvm::Expected<NdVar> {
    if (Next > std::numeric_limits<uint64_t>::max() - 8)
      return invalid("pairing temporary namespace exhausted");
    LowOp O;
    O.Opcode = Opcode;
    O.Output = NdVar::tmp(Next, 1);
    Next += 8;
    O.addInput(L);
    O.addInput(R);
    Result.Expressions.push_back(O);
    return O.Output;
  };
  auto Predicate = Boolean(NdOp::BOOL_AND, *AP, *BP);
  if (!Predicate)
    return Predicate.takeError();
  // Retain both input bindings so the checker validates both projections.
  // Substituting a shared temporary could erase the candidate's location.
  for (const auto &[Candidate, Original] : Shared) {
    auto Equal = Boolean(NdOp::INT_EQUAL, LeftInputs.at(Original),
                         RightInputs.at(Candidate));
    if (!Equal)
      return Equal.takeError();
    auto And = Boolean(NdOp::BOOL_AND, *Predicate, *Equal);
    if (!And)
      return And.takeError();
    *Predicate = *And;
  }
  Result.Predicate = *Predicate;
  for (auto V : A.Rank) {
    auto Mapped = Left.use(V);
    if (!Mapped)
      return Mapped.takeError();
    Result.Rank.push_back(*Mapped);
  }
  // Candidate rank expressions are retained above. Their operands still need
  // valid bindings, although only the original rank is selected for this plan.
  for (auto V : B.Rank) {
    auto Mapped = Right.use(V);
    if (!Mapped)
      return Mapped.takeError();
  }
  return Result;
}

} // namespace

llvm::Expected<LowIRLoopRefinementPlan>
pairLowIRLoopRefinementPlans(const LowIRLoopRefinementPlan &Original,
                             const LowIRLoopRefinementPlan &Candidate,
                             llvm::ArrayRef<LowIRLoopCutpointPair> Pairings,
                             uint64_t MaxMetadata) {
  const auto Charge = [&](uint64_t Count) {
    if (Count > MaxMetadata)
      return false;
    MaxMetadata -= Count;
    return true;
  };
  const auto CountPlan = [&](const LowIRLoopRefinementPlan &Plan) {
    if (!Charge(Plan.Cutpoints.size()))
      return false;
    for (const auto &C : Plan.Cutpoints)
      if (!Charge(C.Inputs.size()) || !Charge(C.Expressions.size()) ||
          !Charge(C.OriginalState.size()) || !Charge(C.CandidateState.size()) ||
          !Charge(C.Rank.size()) || !Charge(C.OriginalGuards.size()) ||
          !Charge(C.CandidateGuards.size()))
        return false;
    return true;
  };
  if (!Charge(Pairings.size()) || !CountPlan(Original) || !CountPlan(Candidate))
    return invalid("loop pairing metadata budget exhausted");
  for (const auto &P : Pairings)
    if (!Charge(P.SharedInputs.size()))
      return invalid("loop pairing metadata budget exhausted");
  if (Pairings.empty() || Pairings.size() != Original.Cutpoints.size() ||
      Pairings.size() != Candidate.Cutpoints.size())
    return invalid("pairings must cover both nonempty plans exactly once");
  std::map<va_t, const LowIRLoopCutpoint *> Left, Right;
  const auto Index = [](const LowIRLoopRefinementPlan &Plan,
                        std::map<va_t, const LowIRLoopCutpoint *> &Out) {
    for (const auto &C : Plan.Cutpoints)
      if (C.OriginalAddress != C.CandidateAddress ||
          !sameAssignments(C.OriginalState, C.CandidateState) ||
          !sameGuards(C.OriginalGuards, C.CandidateGuards) ||
          !Out.emplace(C.OriginalAddress, &C).second)
        return false;
    return true;
  };
  if (!Index(Original, Left) || !Index(Candidate, Right))
    return invalid("pairing requires self-relation plans with unique cuts");
  LowIRLoopRefinementPlan Result;
  for (const auto &P : Pairings) {
    auto A = Left.find(P.OriginalAddress), B = Right.find(P.CandidateAddress);
    if (A == Left.end() || B == Right.end())
      return invalid("missing or repeated cutpoint pairing");
    auto Cut = pairCut(*A->second, *B->second, P);
    if (!Cut)
      return Cut.takeError();
    Result.Cutpoints.push_back(std::move(*Cut));
    Left.erase(A);
    Right.erase(B);
  }
  return Result;
}

} // namespace neverd::analysis
