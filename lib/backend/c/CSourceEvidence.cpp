//===- CSourceEvidence.cpp - Occurrence-bound source evidence ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Project observations without changing the program's execution contract.

#include "neverd/backend/c/CSourceEvidence.h"

#include "neverd/backend/llvm/LLVMSourceMap.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Instructions.h"

#include <map>
#include <set>

namespace neverd {
namespace {
using Key = std::pair<va_t, sigs::LibraryOccurrence>;

std::string escapeComment(llvm::StringRef Text) {
  std::string Out;
  for (size_t I = 0; I < Text.size(); ++I) {
    unsigned char C = Text[I];
    if (C == '*' && I + 1 < Text.size() && Text[I + 1] == '/')
      Out += "* ";
    else if (C < 32 || C >= 127) {
      Out += "<";
      Out += llvm::utohexstr(C, true);
      Out += ">";
    } else
      Out += char(C);
  }
  return Out;
}

std::string identifier(llvm::StringRef Name) {
  std::string Out = "observed_";
  for (char C : Name.take_front(96))
    Out += llvm::isAlnum(C) ? C : '_';
  return Out;
}
} // namespace

void nameLLVMSourceEvidence(const LLVMSourceMap &Sources,
                            llvm::ArrayRef<CSourceEvidence> Evidence) {
  using NameKey = std::pair<Key, CSourceEvidence::ValueRole>;
  std::map<NameKey, std::set<std::string>> Names;
  for (const auto &E : Evidence)
    if (!E.ResultName.empty())
      Names[{{E.Function, E.Occurrence}, E.Role}].insert(E.ResultName);
  std::map<llvm::Instruction *, std::set<std::string>> Candidates;
  for (const auto &O : Sources.Observations) {
    auto *Instruction = llvm::dyn_cast_or_null<llvm::Instruction>(O.Value);
    auto F = Sources.Functions.find(O.Function);
    if (!Instruction || F == Sources.Functions.end() ||
        F->second != Instruction->getFunction())
      continue;
    auto Role = CSourceEvidence::ValueRole::CallResult;
    llvm::Instruction *Value = nullptr;
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Instruction))
      Value = Call;
    else if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Instruction)) {
      Role = CSourceEvidence::ValueRole::StoredValue;
      Value = llvm::dyn_cast<llvm::Instruction>(Store->getValueOperand());
    }
    if (!Value || Value->getType()->isVoidTy() ||
        Value->getFunction() != Instruction->getFunction())
      continue;
    auto At = Names.find({{O.Function, O.Occurrence}, Role});
    if (At != Names.end())
      Candidates[Value].insert(At->second.begin(), At->second.end());
  }
  for (auto &[Call, Names] : Candidates)
    if (Names.size() == 1)
      Call->setName(identifier(*Names.begin()));
}

CSourceEvidenceResult
annotateCSourceEvidence(llvm::StringRef Source, const CSourceMap &Map,
                        llvm::ArrayRef<CSourceEvidence> Evidence) {
  std::map<Key, std::vector<CSourceSpan>> Locations;
  for (const auto &Anchor : Map.Anchors) {
    if (Anchor.Span.Begin >= Anchor.Span.End || Anchor.Span.End > Source.size())
      continue;
    for (const auto &Occurrence : Anchor.Occurrences)
      Locations[{Anchor.Function, Occurrence}].push_back(Anchor.Span);
  }
  CSourceEvidenceResult Result;
  std::map<size_t, std::set<std::string>> Insertions;
  for (const auto &E : Evidence) {
    auto At = Locations.find({E.Function, E.Occurrence});
    if (E.Text.empty() || At == Locations.end() || At->second.empty()) {
      ++Result.Unmapped;
      continue;
    }
    auto Span =
        *llvm::min_element(At->second, [](const auto &L, const auto &R) {
          return L.End - L.Begin < R.End - R.Begin;
        });
    // An expression and its containing statement may record the same exact
    // call. Prefer its complete inner event; disjoint copies stay ambiguous.
    if (llvm::any_of(At->second, [&](const auto &S) {
          return S.Begin > Span.Begin || S.End < Span.End;
        })) {
      ++Result.Unmapped;
      continue;
    }
    size_t Begin = Span.Begin;
    size_t Line = Source.rfind('\n', Begin);
    Line = Line == llvm::StringRef::npos ? 0 : Line + 1;
    if (Source.slice(Line, Begin).trim().empty())
      Begin = Line;
    ++Result.Mapped;
    Insertions[Begin].insert(escapeComment(E.Text));
  }
  size_t Copied = 0;
  for (const auto &[Offset, Lines] : Insertions) {
    Result.Source += Source.slice(Copied, Offset);
    bool AtLineStart = !Offset || Source[Offset - 1] == '\n';
    for (const auto &Line : Lines)
      Result.Source += "/* NeverD observed: " + Line + " */" +
                       (AtLineStart ? "\n" : " ");
    Copied = Offset;
  }
  Result.Source += Source.drop_front(Copied);
  return Result;
}
} // namespace neverd
