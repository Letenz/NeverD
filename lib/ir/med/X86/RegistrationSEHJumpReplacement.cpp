//===- RegistrationSEHJumpReplacement.cpp - Replaced capture closure ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds abandoned jumps to exact callback transfers and completed outcomes.

#include "RegistrationSEHJumpPrivate.h"

#include "neverd/Limits.h"
#include "neverd/ir/RegistrationState.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace neverd {

bool hasCompleteSEHJumpReplacements(const RegistrationStateAnalysis &States,
                                    size_t &Work) {
  const auto &Resumes = States.SEHJumpResumptions;
  if (Resumes.size() > limits::kMaxRegistrationEHStateWork ||
      Work > limits::kMaxRegistrationEHStateWork - Resumes.size())
    return false;
  Work += Resumes.size();
  std::map<RegistrationSEHJumpReplacement, std::vector<size_t>> Occurrences;
  for (size_t I = 0; I != Resumes.size(); ++I) {
    const auto &Resume = Resumes[I];
    Occurrences[{Resume.Address, Resume.OpSeq, Resume.EntryVA,
                 Resume.CaptureCall}]
        .push_back(I);
  }
  std::vector<std::set<size_t>> Parents(Resumes.size());
  std::vector<size_t> Remaining(Resumes.size());
  std::vector<size_t> Ready;
  for (size_t I = 0; I != Resumes.size(); ++I) {
    const auto &Resume = Resumes[I];
    const auto &Replacements = Resume.Replacements;
    if (Replacements.size() > limits::kMaxRegistrationEHStateWork ||
        Work > limits::kMaxRegistrationEHStateWork - Replacements.size())
      return false;
    Work += Replacements.size();
    if (!llvm::is_sorted(Replacements) ||
        std::adjacent_find(Replacements.begin(), Replacements.end()) !=
            Replacements.end())
      return false;
    std::set<size_t> Children;
    for (const auto &Replacement : Replacements) {
      const auto Targets = Occurrences.find(Replacement);
      if (!Replacement.EntryVA ||
          Replacement.CaptureCall == Resume.CaptureCall ||
          Targets == Occurrences.end())
        return false;
      for (size_t Target : Targets->second) {
        if (++Work > limits::kMaxRegistrationEHStateWork || Target == I)
          return false;
        Children.insert(Target);
        Parents[Target].insert(I);
      }
    }
    Remaining[I] = Children.size();
    if (!Remaining[I]) {
      if (Resume.ResumedLevels.empty() && Resume.SelectedScopes.empty())
        return false;
      Ready.push_back(I);
    }
  }
  size_t Completed = 0;
  while (!Ready.empty()) {
    const size_t Child = Ready.back();
    Ready.pop_back();
    ++Completed;
    for (size_t Parent : Parents[Child]) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (!--Remaining[Parent])
        Ready.push_back(Parent);
    }
  }
  return Completed == Resumes.size();
}

} // namespace neverd
