//===- SourceAlignment.cpp - Bounded exact source-line alignment ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Linear-space LCS over exact interned source lines, with explicit stop
/// states.
//===----------------------------------------------------------------------===//

#include "neverd/diff/SourceAlignment.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <utility>

namespace neverd::diff {
namespace {
struct Interrupted {
  ComparisonState State;
};

class Alignment {
  const SourceAlignmentOptions &Options;
  const ComparisonControl &Control;
  SourceAlignment &Result;
  std::vector<uint64_t> A, B;
  std::vector<std::pair<size_t, size_t>> Matches;

  void charge(uint64_t Amount = 1) {
    if (auto Stop = Control.stop())
      throw Interrupted{*Stop};
    if (Amount > UINT64_MAX - Result.Work ||
        (Options.MaxWork && Amount > Options.MaxWork - Result.Work))
      throw Interrupted{ComparisonState::ResourceLimited};
    Result.Work += Amount;
  }

  bool equal(size_t I, size_t J) {
    charge();
    return A[I] == B[J];
  }

  // One in-place row of the LCS recurrence; reverse reads suffixes without
  // copying input. Only two such rows survive a split search, and both are
  // released before recursion. Recursion halves A, so its depth is logarithmic.
  std::vector<size_t> scores(size_t ABegin, size_t AEnd, size_t BBegin,
                             size_t BEnd, bool Reverse) {
    std::vector<size_t> Row(BEnd - BBegin + 1);
    for (size_t I = 0; I < AEnd - ABegin; ++I) {
      size_t Diagonal = 0;
      for (size_t J = 1; J <= BEnd - BBegin; ++J) {
        const auto Previous = Row[J];
        Row[J] = equal(Reverse ? AEnd - I - 1 : ABegin + I,
                       Reverse ? BEnd - J : BBegin + J - 1)
                     ? Diagonal + 1
                     : std::max(Row[J], Row[J - 1]);
        Diagonal = Previous;
      }
      if (Control.Progress)
        Control.Progress("source_alignment", Result.Work, 0);
    }
    return Row;
  }

  size_t split(size_t ABegin, size_t AMiddle, size_t AEnd, size_t BBegin,
               size_t BEnd) {
    const auto Forward = scores(ABegin, AMiddle, BBegin, BEnd, false);
    const auto Backward = scores(AMiddle, AEnd, BBegin, BEnd, true);
    size_t Best = 0, Position = 0;
    for (size_t I = 0; I <= BEnd - BBegin; ++I) {
      charge();
      const auto Score = Forward[I] + Backward[BEnd - BBegin - I];
      if (Score > Best) {
        Best = Score;
        Position = I;
      }
    }
    return BBegin + Position;
  }

  void match(size_t ABegin, size_t AEnd, size_t BBegin, size_t BEnd) {
    charge(0);
    while (ABegin < AEnd && BBegin < BEnd && equal(ABegin, BBegin))
      Matches.emplace_back(ABegin++, BBegin++);
    const size_t OriginalAEnd = AEnd;
    while (ABegin < AEnd && BBegin < BEnd && equal(AEnd - 1, BEnd - 1)) {
      --AEnd;
      --BEnd;
    }
    if (ABegin < AEnd && BBegin < BEnd) {
      if (AEnd - ABegin == 1) {
        for (size_t J = BBegin; J < BEnd; ++J)
          if (equal(ABegin, J)) {
            Matches.emplace_back(ABegin, J);
            break;
          }
      } else {
        const size_t AMiddle = ABegin + (AEnd - ABegin) / 2;
        const size_t BMiddle = split(ABegin, AMiddle, AEnd, BBegin, BEnd);
        match(ABegin, AMiddle, BBegin, BMiddle);
        match(AMiddle, AEnd, BMiddle, BEnd);
      }
    }
    for (size_t I = 0; I < OriginalAEnd - AEnd; ++I) {
      charge();
      Matches.emplace_back(AEnd + I, BEnd + I);
    }
  }

  void row(uint64_t Primary, uint64_t Secondary, SourceLineKind Kind) {
    charge();
    Result.Rows.push_back({Primary, Secondary, Kind});
  }

public:
  Alignment(const SourceAlignmentOptions &Options,
            const ComparisonControl &Control, SourceAlignment &Result)
      : Options(Options), Control(Control), Result(Result) {}

  void run(llvm::ArrayRef<llvm::StringRef> Primary,
           llvm::ArrayRef<llvm::StringRef> Secondary) {
    llvm::StringMap<uint64_t> Symbols;
    const auto Intern = [&](llvm::ArrayRef<llvm::StringRef> Lines,
                            std::vector<uint64_t> &IDs) {
      for (const auto Line : Lines) {
        charge(Line.size());
        charge();
        // StringMap checks complete bytes even if two hashes collide.
        IDs.push_back(Symbols.try_emplace(Line, Symbols.size()).first->second);
        if (Control.Progress)
          Control.Progress("source_lines", Result.Work, 0);
      }
    };
    Intern(Primary, A);
    Intern(Secondary, B);
    match(0, A.size(), 0, B.size());
    size_t I = 0, J = 0;
    const auto Gap = [&](size_t AEnd, size_t BEnd) {
      while (I < AEnd && J < BEnd)
        row(I++, J++, SourceLineKind::Changed);
      while (I < AEnd)
        row(I++, NoSourceLine, SourceLineKind::Deleted);
      while (J < BEnd)
        row(NoSourceLine, J++, SourceLineKind::Inserted);
    };
    for (const auto &[AI, BI] : Matches) {
      Gap(AI, BI);
      row(I++, J++, SourceLineKind::Unchanged);
    }
    Gap(A.size(), B.size());
    charge(0);
    Result.ComputationComplete = true;
  }
};
} // namespace

llvm::StringRef sourceLineKindName(SourceLineKind Kind) {
  switch (Kind) {
  case SourceLineKind::Unchanged:
    return "unchanged";
  case SourceLineKind::Changed:
    return "changed";
  case SourceLineKind::Inserted:
    return "inserted";
  case SourceLineKind::Deleted:
    return "deleted";
  }
  llvm_unreachable("invalid source line kind");
}

SourceAlignment alignSourceLines(llvm::ArrayRef<llvm::StringRef> Primary,
                                 llvm::ArrayRef<llvm::StringRef> Secondary,
                                 const SourceAlignmentOptions &Options,
                                 const ComparisonControl &Control) {
  SourceAlignment Result;
  try {
    Alignment(Options, Control, Result).run(Primary, Secondary);
  } catch (const Interrupted &Stop) {
    Result.State = Stop.State;
    Result.Rows.clear();
  }
  return Result;
}
} // namespace neverd::diff
