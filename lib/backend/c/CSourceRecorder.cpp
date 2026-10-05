//===- CSourceRecorder.cpp - Verify source spans without editing C --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CSourceRecorder.h"

#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <set>

namespace neverd {

CSourceRecorder::CSourceRecorder(CSourceMap &Map, llvm::StringRef Ordinary)
    : Map(Map) {
  Map.Regions.clear();
  if (Map.Recognitions)
    for (size_t I = 0; I < Map.Recognitions->size(); ++I)
      Map.Regions.push_back({I, {}, false});
  for (size_t Salt = 0;; ++Salt) {
    Prefix = "\x1eND:" + std::to_string(Salt) + ':';
    if (!Ordinary.contains(Prefix))
      break;
  }
}

void CSourceRecorder::prepareHighSources() {
  if (!Map.HighSources || !Map.Recognitions)
    return;
  using Key = std::pair<va_t, const HighExpr *>;
  std::map<Key, std::set<sigs::LibraryOccurrence>> Origins;
  std::map<Key, std::shared_ptr<const HighExpr>> Alive;
  for (const auto &Source : *Map.HighSources)
    if (auto Expr = Source.Expression.lock()) {
      Key K{Source.Function, Expr.get()};
      Origins[K].insert(Source.Occurrence);
      Alive[K] = std::move(Expr);
    }
  for (size_t I = 0; I < Map.Recognitions->size(); ++I) {
    const auto &Match = (*Map.Recognitions)[I];
    if (!Match.Isolated || !Match.ResultOccurrence ||
        Match.Scope != sigs::LibraryFeatureScope::InlineExpression)
      continue;
    for (const auto &[K, Expr] : Alive) {
      if (K.first != Match.Function ||
          !Origins[K].contains(*Match.ResultOccurrence))
        continue;
      std::set<sigs::LibraryOccurrence> Covered;
      std::set<const HighExpr *> Seen;
      std::vector<const HighExpr *> Pending{Expr.get()};
      size_t Budget = 4096;
      while (!Pending.empty() && Budget) {
        --Budget;
        const HighExpr *Current = Pending.back();
        Pending.pop_back();
        if (!Seen.insert(Current).second)
          continue;
        if (auto At = Origins.find({Match.Function, Current});
            At != Origins.end())
          Covered.insert(At->second.begin(), At->second.end());
        for (const auto &Child : Current->Operands)
          if (Child)
            Pending.push_back(Child.get());
      }
      if (Pending.empty() &&
          std::includes(Covered.begin(), Covered.end(),
                        Match.Occurrences.begin(), Match.Occurrences.end()))
        HighRegions.emplace(K, I);
    }
  }
}

void CSourceRecorder::prepareLLVMSources(const LLVMSourceMap &Sources) {
  for (const auto &[Entry, Handle] : Sources.Functions)
    if (auto *Function = llvm::dyn_cast_or_null<llvm::Function>(Handle))
      LLVMFunctions.emplace(Function, Entry);
  if (!Map.Recognitions)
    return;
  std::set<const llvm::Value *> Ambiguous;
  for (const auto &Observation : Sources.Observations) {
    auto *Instruction =
        llvm::dyn_cast_or_null<llvm::Instruction>(Observation.Value);
    if (!Instruction)
      continue;
    auto Function = LLVMFunctions.find(Instruction->getFunction());
    if (Function == LLVMFunctions.end() ||
        Function->second != Observation.Function)
      continue;
    for (size_t I = 0; I < Map.Recognitions->size(); ++I) {
      const auto &Match = (*Map.Recognitions)[I];
      if (Match.Function != Observation.Function || !Match.Isolated ||
          Match.Scope != sigs::LibraryFeatureScope::InlineExpression ||
          !std::binary_search(Match.Occurrences.begin(),
                              Match.Occurrences.end(), Observation.Occurrence))
        continue;
      auto [At, Added] = LLVMRegions.try_emplace(Instruction, Event{I, {}});
      if (At->second.Region != I)
        Ambiguous.insert(Instruction);
      else
        At->second.Coverage.push_back(Observation.Occurrence);
    }
  }
  for (const auto *Value : Ambiguous)
    LLVMRegions.erase(Value);
}

size_t CSourceRecorder::event(size_t Region,
                              std::vector<sigs::LibraryOccurrence> Coverage) {
  Events.push_back({Region, std::move(Coverage)});
  return Events.size() - 1;
}

std::string CSourceRecorder::begin(size_t Event) const {
  return Prefix + std::to_string(Event) + "B\x1f";
}

std::string CSourceRecorder::end(size_t Event) const {
  return Prefix + std::to_string(Event) + "E\x1f";
}

std::string CSourceRecorder::expression(va_t Function, const HighExpr &Expr,
                                        std::string Text) {
  auto It = HighRegions.find({Function, &Expr});
  if (It == HighRegions.end())
    return Text;
  size_t Event = event(It->second, (*Map.Recognitions)[It->second].Occurrences);
  return begin(Event) + Text + end(Event);
}

std::optional<size_t> CSourceRecorder::function(va_t Entry) {
  if (Map.Recognitions)
    for (size_t I = 0; I < Map.Recognitions->size(); ++I) {
      const auto &Match = (*Map.Recognitions)[I];
      if (Match.Function == Entry && Match.Isolated &&
          Match.Scope == sigs::LibraryFeatureScope::WholeFunction)
        return event(I, Match.Occurrences);
    }
  return std::nullopt;
}

std::optional<size_t>
CSourceRecorder::function(const llvm::Function &Function) {
  auto I = LLVMFunctions.find(&Function);
  return I == LLVMFunctions.end() ? std::nullopt : function(I->second);
}

std::optional<size_t>
CSourceRecorder::instruction(const llvm::Instruction &Value) {
  auto I = LLVMRegions.find(&Value);
  return I == LLVMRegions.end() ? std::nullopt
                                : std::optional<size_t>(event(
                                      I->second.Region, I->second.Coverage));
}

std::string CSourceRecorder::expression(const llvm::Instruction &Value,
                                        std::string Text) {
  auto Event = instruction(Value);
  return Event ? begin(*Event) + Text + end(*Event) : Text;
}

bool CSourceRecorder::finish(llvm::StringRef Annotated,
                             llvm::StringRef Ordinary) {
  std::vector<CSourceRegion> Candidate = Map.Regions;
  std::vector<std::set<sigs::LibraryOccurrence>> Covered(Candidate.size());
  std::string Clean;
  struct Frame {
    size_t Event;
    size_t Begin;
  };
  std::vector<Frame> Stack;
  while (!Annotated.empty()) {
    size_t At = Annotated.find(Prefix);
    if (At == llvm::StringRef::npos) {
      Clean.append(Annotated.data(), Annotated.size());
      break;
    }
    Clean.append(Annotated.data(), At);
    Annotated = Annotated.drop_front(At + Prefix.size());
    size_t Stop = Annotated.find('\x1f');
    if (Stop == llvm::StringRef::npos || Stop < 2)
      return false;
    llvm::StringRef Token = Annotated.take_front(Stop);
    size_t Event = 0;
    if (Token.drop_back().getAsInteger(10, Event) || Event >= Events.size())
      return false;
    if (Token.back() == 'B') {
      Stack.push_back({Event, Clean.size()});
    } else if (Token.back() == 'E') {
      if (Stack.empty() || Stack.back().Event != Event)
        return false;
      const auto Open = Stack.back();
      Stack.pop_back();
      if (Open.Begin != Clean.size()) {
        const auto &Target = Events[Event];
        Candidate[Target.Region].Spans.push_back({Open.Begin, Clean.size()});
        Covered[Target.Region].insert(Target.Coverage.begin(),
                                      Target.Coverage.end());
      }
    } else {
      return false;
    }
    Annotated = Annotated.drop_front(Stop + 1);
  }
  if (!Stack.empty() || Clean != Ordinary)
    return false;
  for (auto &Region : Candidate) {
    auto &Spans = Region.Spans;
    std::sort(Spans.begin(), Spans.end(), [](auto A, auto B) {
      return std::tie(A.Begin, A.End) < std::tie(B.Begin, B.End);
    });
    Spans.erase(std::unique(Spans.begin(), Spans.end()), Spans.end());
    const auto &Required = (*Map.Recognitions)[Region.Recognition].Occurrences;
    const auto &Found = Covered[Region.Recognition];
    Region.Mapped =
        !Spans.empty() && std::includes(Found.begin(), Found.end(),
                                        Required.begin(), Required.end());
    std::vector<CSourceSpan> Merged;
    for (auto Span : Spans) {
      if (!Merged.empty() && Span.Begin <= Merged.back().End)
        Merged.back().End = std::max(Merged.back().End, Span.End);
      else
        Merged.push_back(Span);
    }
    Spans = std::move(Merged);
    if (!Region.Mapped)
      Spans.clear();
  }
  for (size_t I = 0; I < Candidate.size(); ++I)
    for (size_t J = I + 1; J < Candidate.size(); ++J)
      for (auto A : Candidate[I].Spans)
        for (auto B : Candidate[J].Spans)
          if (A.Begin < B.End && B.Begin < A.End)
            Candidate[I].Mapped = Candidate[J].Mapped = false;
  for (auto &Region : Candidate)
    if (!Region.Mapped)
      Region.Spans.clear();
  Map.Regions = std::move(Candidate);
  return true;
}

} // namespace neverd
