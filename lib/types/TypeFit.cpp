//===- TypeFit.cpp - Local types that fit a pointer's accesses ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/TypeFit.h"

#include "neverd/types/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace neverd::types {
namespace {

bool hasElement(llvm::ArrayRef<MemberStep> Path) {
  return llvm::any_of(
      Path, [](const MemberStep &Step) { return Step.Element.has_value(); });
}

bool isRecordType(const LocalType &Type) {
  return (Type.Kind == TypeKind::Struct || Type.Kind == TypeKind::Union) &&
         Type.Record && Type.Record->Defined && Type.Record->Size;
}

/// Lands accesses among the members of a library's records.
class Lander {
public:
  explicit Lander(const TypeLibrary &Library) : Library(Library) {}

  /// \p Type through the typedefs that name it.
  const TypeExpr &shapeOf(const TypeExpr &Type) const {
    const TypeExpr *Shape = &Type;
    for (unsigned Steps = 0; Shape->Kind == TypeExprKind::Named &&
                             Steps < ::neverd::limits::types::kTypeFitMaxDepth;
         ++Steps) {
      const LocalType *Named = Library.find(Shape->Name);
      if (!Named || Named->Kind != TypeKind::Typedef || !Named->Target)
        break;
      Shape = Named->Target.get();
    }
    return *Shape;
  }

  /// The defined record \p Name names, through typedefs.
  const LocalType *recordNamed(llvm::StringRef Name) const {
    const LocalType *Named = Library.resolve(Name);
    return Named && isRecordType(*Named) ? Named : nullptr;
  }

  /// The record \p Type lays out, in place or by name.
  const RecordDef *recordOf(const TypeExpr &Type) const {
    const TypeExpr &Shape = shapeOf(Type);
    if (Shape.Kind == TypeExprKind::Record)
      return Shape.Record.get();
    if (Shape.Kind != TypeExprKind::Named)
      return nullptr;
    const LocalType *Named = recordNamed(Shape.Name);
    return Named ? Named->Record.get() : nullptr;
  }

  uint64_t bytesOf(const TypeExpr &Type) const {
    return Library.sizeOf(Type).value_or(0);
  }

  bool aggregate(const TypeExpr &Type) const {
    return recordOf(Type) || shapeOf(Type).Kind == TypeExprKind::Array;
  }

  /// Whether \p A lands better than \p B.
  bool better(const MemberAt &A, const MemberAt &B) const {
    if (A.Fit != B.Fit)
      return A.Fit < B.Fit;
    // A scalar is what reads as wide a record or array as it begins.
    if (A.Fit == AccessFit::Exact) {
      const bool AScalar = A.Type && !aggregate(*A.Type);
      const bool BScalar = B.Type && !aggregate(*B.Type);
      if (AScalar != BScalar)
        return AScalar;
    }
    const bool AElement = hasElement(A.Path), BElement = hasElement(B.Path);
    if (AElement != BElement)
      return !AElement;
    if (A.Bitfield != B.Bitfield)
      return !A.Bitfield;
    return false;
  }

  /// Where \p Bytes at \p Rel land in the item of \p Type and \p Size
  /// bytes that begins \p Base bytes into the outermost record, \p Path
  /// leading to it.
  MemberAt inItem(const TypeExprRef &Type, uint64_t Size, uint64_t Base,
                  std::vector<MemberStep> &Path, uint64_t Rel, uint64_t Bytes,
                  unsigned Depth) const {
    MemberAt Whole;
    Whole.Path = Path;
    Whole.MemberOffset = Base;
    Whole.MemberBytes = Size;
    Whole.Type = Type;
    const TypeExpr &Shape = shapeOf(*Type);
    const RecordDef *Record = recordOf(Shape);
    const bool Array = Shape.Kind == TypeExprKind::Array && Shape.Element;
    if (Depth >= ::neverd::limits::types::kTypeFitMaxDepth ||
        (!Record && !Array)) {
      Whole.Fit = Rel == 0 && (Bytes == 0 || Bytes == Size) ? AccessFit::Exact
                  : Rel == 0                                ? AccessFit::Start
                                                            : AccessFit::Inside;
      return Whole;
    }
    if (Bytes == 0 && Rel == 0) {
      Whole.Fit = AccessFit::Exact;
      return Whole;
    }
    if (Bytes && Rel + Bytes > Size) {
      Whole.Fit = Rel == 0 ? AccessFit::Start : AccessFit::Inside;
      return Whole;
    }
    MemberAt Inner = Record ? inRecord(*Record, Base, Path, Rel, Bytes, Depth)
                            : inArray(Shape, Base, Path, Rel, Bytes, Depth);
    if (Bytes && Rel == 0 && Bytes == Size) {
      // The access reads the item whole: a scalar as wide that begins it is
      // what it reads, otherwise the item is.
      Whole.Fit = AccessFit::Exact;
      if (Inner.Fit == AccessFit::Exact && Inner.Type &&
          !aggregate(*Inner.Type))
        return Inner;
      return Whole;
    }
    return Inner;
  }

  MemberAt inArray(const TypeExpr &Shape, uint64_t Base,
                   std::vector<MemberStep> &Path, uint64_t Rel, uint64_t Bytes,
                   unsigned Depth) const {
    const uint64_t Each = bytesOf(*Shape.Element);
    if (!Each) {
      MemberAt At;
      At.Fit = AccessFit::Inside;
      At.Path = Path;
      At.MemberOffset = Base;
      return At;
    }
    const uint64_t Index = Rel / Each;
    Path.push_back({std::string(), Index});
    MemberAt At = inItem(Shape.Element, Each, Base + Index * Each, Path,
                         Rel - Index * Each, Bytes, Depth + 1);
    Path.pop_back();
    return At;
  }

  MemberAt inRecord(const RecordDef &Record, uint64_t Base,
                    std::vector<MemberStep> &Path, uint64_t Rel, uint64_t Bytes,
                    unsigned Depth) const {
    std::optional<MemberAt> Best;
    const std::vector<Member> &Members = Record.Members;
    for (size_t I = 0; I < Members.size(); ++I) {
      const Member &Field = Members[I];
      if (!Field.Type)
        continue;
      const bool Bitfield = Field.isBitfield();
      // A run of bitfields is one storage unit, which the first of them
      // stands for.
      if (Bitfield && !Record.Union && I > 0 && Members[I - 1].isBitfield() &&
          Members[I - 1].StorageBitOffset == Field.StorageBitOffset)
        continue;
      const uint64_t Start =
          (Bitfield ? Field.StorageBitOffset : Field.BitOffset) / 8;
      const uint64_t Size = bytesOf(*Field.Type);
      const bool Holds =
          Rel >= Start && (Rel < Start + Size || (Bytes == 0 && Rel == Start));
      if (!Holds)
        continue;
      MemberAt At;
      if (Bitfield) {
        // The unit takes the name of its first named bitfield.
        const Member *Named = nullptr;
        for (size_t J = I;
             J < Members.size() && Members[J].isBitfield() &&
             Members[J].StorageBitOffset == Field.StorageBitOffset;
             ++J)
          if (!Members[J].Name.empty()) {
            Named = &Members[J];
            break;
          }
        if (!Named)
          continue;
        At.Path = Path;
        At.Path.push_back({Named->Name, std::nullopt});
        At.MemberOffset = Base + Start;
        At.MemberBytes = Size;
        At.Type = Field.Type;
        At.Bitfield = true;
        At.Fit = Rel != Start                  ? AccessFit::Inside
                 : Bytes == 0 || Bytes == Size ? AccessFit::Exact
                                               : AccessFit::Start;
      } else if (Field.isAnonymous()) {
        At = inItem(Field.Type, Size, Base + Start, Path, Rel - Start, Bytes,
                    Depth + 1);
      } else if (!Field.Name.empty()) {
        Path.push_back({Field.Name, std::nullopt});
        At = inItem(Field.Type, Size, Base + Start, Path, Rel - Start, Bytes,
                    Depth + 1);
        Path.pop_back();
      } else {
        continue;
      }
      if (!Best || better(At, *Best))
        Best = std::move(At);
    }
    if (Best)
      return *Best;
    MemberAt Pad;
    Pad.Fit = AccessFit::Padding;
    Pad.Path = Path;
    Pad.MemberOffset = Base;
    return Pad;
  }

  /// Where every member of \p Record and of the records it holds in place
  /// begins, from its start; what a pointer into it points at.
  void memberStarts(const RecordDef &Record, uint64_t Base,
                    std::set<uint64_t> &Starts, unsigned Depth) const {
    if (Depth >= ::neverd::limits::types::kTypeFitMaxDepth)
      return;
    for (const Member &Field : Record.Members) {
      if (Starts.size() >= ::neverd::limits::types::kTypeFitMaxShifts)
        return;
      if (!Field.Type || Field.isBitfield())
        continue;
      const uint64_t Start = Base + Field.BitOffset / 8;
      Starts.insert(Start);
      if (const RecordDef *Inner = recordOf(*Field.Type))
        memberStarts(*Inner, Start, Starts, Depth + 1);
    }
  }

  /// Every named member of \p Record and of the records it holds, with its
  /// path; arrays' elements are not.
  void memberPaths(const RecordDef &Record, std::vector<MemberStep> &Path,
                   std::vector<std::vector<MemberStep>> &Paths,
                   unsigned Depth) const {
    if (Depth >= ::neverd::limits::types::kTypeFitMaxDepth)
      return;
    for (const Member &Field : Record.Members) {
      if (!Field.Type)
        continue;
      if (Field.isAnonymous()) {
        memberPaths(*Field.Type->Record, Path, Paths, Depth + 1);
        continue;
      }
      if (Field.Name.empty())
        continue;
      Path.push_back({Field.Name, std::nullopt});
      Paths.push_back(Path);
      if (const RecordDef *Inner = recordOf(*Field.Type))
        memberPaths(*Inner, Path, Paths, Depth + 1);
      Path.pop_back();
    }
  }

private:
  const TypeLibrary &Library;
};

// Fuzzy matching, after the scoring of fzf's second algorithm.
constexpr int ScoreMatch = 16;
constexpr int ScoreGapStart = -3;
constexpr int ScoreGapExtension = -1;
constexpr int BonusBoundary = 8;
constexpr int BonusCamel = 7;
constexpr int BonusConsecutive = 4;
constexpr int BonusFirstCharMultiplier = 2;
constexpr int BonusNameStart = 8;
constexpr int BonusExactCase = 1;
constexpr int BonusWholeName = 32;
constexpr int NoScore = std::numeric_limits<int>::min() / 4;

/// Where the name proper begins: past its leading underscores.
size_t nameStart(llvm::StringRef Text) {
  size_t I = 0;
  while (I < Text.size() && Text[I] == '_')
    ++I;
  return I == Text.size() ? 0 : I;
}

/// What matching a pattern character at \p I of \p Text earns besides the
/// match itself.
int boundaryBonus(llvm::StringRef Text, size_t I) {
  if (I == 0)
    return BonusBoundary;
  const char Prev = Text[I - 1], Cur = Text[I];
  if (!llvm::isAlnum(Prev))
    return BonusBoundary;
  if ((llvm::isLower(Prev) && llvm::isUpper(Cur)) ||
      (!llvm::isDigit(Prev) && llvm::isDigit(Cur)))
    return BonusCamel;
  return 0;
}

std::vector<MatchRun> runsOf(llvm::ArrayRef<uint32_t> Positions) {
  std::vector<MatchRun> Runs;
  for (uint32_t Position : Positions) {
    if (!Runs.empty() && Runs.back().Begin + Runs.back().Length == Position)
      ++Runs.back().Length;
    else
      Runs.push_back({Position, 1});
  }
  return Runs;
}

/// \p Text as a key that ignores case.
std::string lowered(llvm::StringRef Text) { return Text.lower(); }

int originRank(TypeOrigin Origin) {
  // The program's own types before those a header library adds.
  return Origin == TypeOrigin::Header ? 1 : 0;
}

/// The best match of \p Pattern among \p Names, and which.
std::optional<std::pair<FuzzyMatch, size_t>>
bestMatch(llvm::StringRef Pattern, llvm::ArrayRef<std::string> Names) {
  std::optional<std::pair<FuzzyMatch, size_t>> Best;
  for (size_t I = 0; I < Names.size(); ++I)
    if (std::optional<FuzzyMatch> Match = fuzzyMatch(Pattern, Names[I]))
      if (!Best || Match->Score > Best->first.Score)
        Best = std::make_pair(std::move(*Match), I);
  return Best;
}

/// What an access landing counts toward a record: exactly on a named member
/// most, on an array's element a little less, as an array of bytes holds
/// any byte; in part, or on padding, against.
constexpr int64_t ExactWeight = 4;
constexpr int64_t ElementWeight = 3;
constexpr int64_t PartialWeight = -3;
constexpr int64_t PaddingWeight = -4;

/// The counts of how \p Fits landed.
struct Counts {
  unsigned Exact = 0, ExactElements = 0, Partial = 0, Padding = 0, Outside = 0;
  int64_t score() const {
    return ExactWeight * Exact + ElementWeight * ExactElements +
           PartialWeight * Partial + PaddingWeight * Padding;
  }
  /// Records that hold every access first, then the better score, then
  /// fewer landing otherwise than exactly.
  auto key() const {
    return std::make_tuple(Outside, -score(), Partial + Padding,
                           -static_cast<int64_t>(Exact));
  }
};

Counts countFits(llvm::ArrayRef<MemberAt> Fits) {
  Counts C;
  for (const MemberAt &At : Fits)
    switch (At.Fit) {
    case AccessFit::Exact:
      ++(hasElement(At.Path) ? C.ExactElements : C.Exact);
      break;
    case AccessFit::Start:
    case AccessFit::Inside:
      ++C.Partial;
      break;
    case AccessFit::Padding:
      ++C.Padding;
      break;
    case AccessFit::Outside:
      ++C.Outside;
      break;
    }
  return C;
}

/// Lands \p Query's accesses in \p Record, the pointer at its start or,
/// when they reach before it, at the member start they fit best.
void judge(const TypeLibrary &Library, const Lander &Land,
           const RecordDef &Record, const FitQuery &Query, TypeFit &Fit) {
  auto LandAt = [&](uint64_t Shift) {
    std::vector<MemberAt> Fits;
    Fits.reserve(Query.Accesses.size());
    for (const PointerAccess &Access : Query.Accesses)
      Fits.push_back(memberAt(Library, Record,
                              Access.Offset + static_cast<int64_t>(Shift),
                              Access.Bytes));
    return Fits;
  };
  std::vector<MemberAt> Best = LandAt(0);
  Counts BestCounts = countFits(Best);
  uint64_t BestShift = 0;
  int64_t Lowest = 0;
  for (const PointerAccess &Access : Query.Accesses)
    Lowest = std::min(Lowest, Access.Offset);
  if (Query.Shifts && Lowest < 0) {
    std::set<uint64_t> Starts;
    Land.memberStarts(Record, 0, Starts, 0);
    for (uint64_t Shift : Starts) {
      if (Shift == 0 || Shift < static_cast<uint64_t>(-Lowest) ||
          Shift >= Record.Size)
        continue;
      std::vector<MemberAt> Fits = LandAt(Shift);
      const Counts C = countFits(Fits);
      if (C.key() < BestCounts.key()) {
        Best = std::move(Fits);
        BestCounts = C;
        BestShift = Shift;
      }
    }
  }
  Fit.Fits = std::move(Best);
  Fit.Shift = BestShift;
  if (BestShift)
    Fit.ShiftPath = memberAt(Library, Record, BestShift, 0).Path;
  Fit.Exact = BestCounts.Exact;
  Fit.ExactElements = BestCounts.ExactElements;
  Fit.Partial = BestCounts.Partial;
  Fit.Padding = BestCounts.Padding;
  Fit.Outside = BestCounts.Outside;
}

} // namespace

MemberAt memberAt(const TypeLibrary &Library, const RecordDef &Record,
                  int64_t Offset, uint64_t Bytes) {
  MemberAt At;
  if (Offset < 0 || static_cast<uint64_t>(Offset) > Record.Size ||
      Record.Size - static_cast<uint64_t>(Offset) <
          std::max<uint64_t>(Bytes, 1))
    return At;
  const Lander Land(Library);
  std::vector<MemberStep> Path;
  const uint64_t Rel = static_cast<uint64_t>(Offset);
  if (Rel == 0 && (Bytes == 0 || Bytes == Record.Size)) {
    // The record itself, unless a scalar as wide begins it.
    At.Fit = AccessFit::Exact;
    At.MemberBytes = Record.Size;
    if (Bytes) {
      MemberAt Inner = Land.inRecord(Record, 0, Path, 0, Bytes, 0);
      if (Inner.Fit == AccessFit::Exact && Inner.Type &&
          !Land.aggregate(*Inner.Type))
        return Inner;
    }
    return At;
  }
  return Land.inRecord(Record, 0, Path, Rel, Bytes, 0);
}

std::string memberPathText(llvm::ArrayRef<MemberStep> Path) {
  std::string Text;
  for (const MemberStep &Step : Path) {
    if (Step.Element) {
      Text += "[" + std::to_string(*Step.Element) + "]";
      continue;
    }
    if (!Text.empty())
      Text += '.';
    Text += Step.Name;
  }
  return Text;
}

std::optional<FuzzyMatch> fuzzyMatch(llvm::StringRef Pattern,
                                     llvm::StringRef Text) {
  if (Pattern.empty())
    return FuzzyMatch();
  const size_t N = Pattern.size(), M = Text.size();
  if (N > M || N > ::neverd::limits::types::kMaxFuzzyPatternBytes ||
      M > ::neverd::limits::types::kMaxFuzzyCandidateBytes)
    return std::nullopt;
  // A pattern that is no subsequence fails before any scoring.
  for (size_t I = 0, J = 0; I < N; ++I, ++J) {
    while (J < M && llvm::toLower(Text[J]) != llvm::toLower(Pattern[I]))
      ++J;
    if (J == M)
      return std::nullopt;
  }
  const size_t Start = nameStart(Text);
  std::vector<int> Bonus(M);
  for (size_t J = 0; J < M; ++J)
    Bonus[J] = boundaryBonus(Text, J) + (J == Start ? BonusNameStart : 0);
  // Score[I][J]: the best score with pattern character I matched at J;
  // From[I][J]: where character I - 1 was matched then.
  std::vector<int> Score(N * M, NoScore);
  std::vector<int> From(N * M, -1);
  for (size_t I = 0; I < N; ++I) {
    const char Want = llvm::toLower(Pattern[I]);
    // The best of the previous row before J - 1, with the gap to J
    // charged, and where it was.
    int Gap = NoScore;
    int GapFrom = -1;
    for (size_t J = I; J < M; ++J) {
      if (I > 0 && J >= 2) {
        const int Extended = Gap == NoScore ? NoScore : Gap + ScoreGapExtension;
        const int Started = Score[(I - 1) * M + J - 2] == NoScore
                                ? NoScore
                                : Score[(I - 1) * M + J - 2] + ScoreGapStart;
        if (Started >= Extended && Started != NoScore) {
          Gap = Started;
          GapFrom = static_cast<int>(J - 2);
        } else {
          Gap = Extended;
        }
      }
      if (llvm::toLower(Text[J]) != Want)
        continue;
      const int Case = Text[J] == Pattern[I] ? BonusExactCase : 0;
      int Best = NoScore, BestFrom = -1;
      if (I == 0) {
        // Skipped characters before the name proper cost a little each.
        const int Skipped =
            J > Start ? static_cast<int>(std::min<size_t>(J - Start, 8)) : 0;
        Best =
            ScoreMatch + Bonus[J] * BonusFirstCharMultiplier + Case - Skipped;
      } else {
        if (J >= 1 && Score[(I - 1) * M + J - 1] != NoScore) {
          Best = Score[(I - 1) * M + J - 1] + ScoreMatch + Case +
                 std::max(Bonus[J], BonusConsecutive);
          BestFrom = static_cast<int>(J - 1);
        }
        if (Gap != NoScore) {
          const int Gapped = Gap + ScoreMatch + Case + Bonus[J];
          if (Gapped > Best) {
            Best = Gapped;
            BestFrom = GapFrom;
          }
        }
      }
      Score[I * M + J] = Best;
      From[I * M + J] = BestFrom;
    }
  }
  int Best = NoScore;
  size_t End = 0;
  for (size_t J = N - 1; J < M; ++J)
    if (Score[(N - 1) * M + J] > Best) {
      Best = Score[(N - 1) * M + J];
      End = J;
    }
  if (Best == NoScore)
    return std::nullopt;
  std::vector<uint32_t> Positions(N);
  for (size_t I = N, J = End; I-- > 0;) {
    Positions[I] = static_cast<uint32_t>(J);
    if (I > 0)
      J = static_cast<size_t>(From[I * M + J]);
  }
  FuzzyMatch Match;
  Match.Score = Best;
  if (Text.drop_front(Start).equals_insensitive(Pattern) ||
      Text.equals_insensitive(Pattern))
    Match.Score += BonusWholeName;
  Match.Runs = runsOf(Positions);
  return Match;
}

std::vector<TypeFit> fitTypes(const TypeLibrary &Library, const FitQuery &Query,
                              size_t *Total) {
  const Lander Land(Library);
  // `Type.member` and `Type->member` search member paths too.
  std::string Text = llvm::StringRef(Query.Text).trim().str();
  for (size_t Arrow; (Arrow = Text.find("->")) != std::string::npos;)
    Text.replace(Arrow, 2, ".");
  llvm::StringRef TypePart = Text, MemberPart;
  bool Members = false;
  if (const size_t Dot = TypePart.find('.'); Dot != llvm::StringRef::npos) {
    MemberPart = TypePart.drop_front(Dot + 1).trim();
    TypePart = TypePart.take_front(Dot).trim();
    Members = !MemberPart.empty();
  }

  // The typedefs of each record, and of pointers to it.
  llvm::StringMap<std::vector<std::string>> Aliases, PointerAliases;
  if (Query.RecordsOnly)
    for (const LocalType *Type : Library.types()) {
      if (Type->Kind != TypeKind::Typedef || !Type->Target)
        continue;
      const TypeExpr &Shape = Land.shapeOf(*Type->Target);
      if (Shape.Kind == TypeExprKind::Named) {
        if (const LocalType *Record = Land.recordNamed(Shape.Name))
          Aliases[Record->Name].push_back(Type->Name);
      } else if (Shape.Kind == TypeExprKind::Pointer && Shape.Element) {
        const TypeExpr &Pointee = Land.shapeOf(*Shape.Element);
        if (Pointee.Kind == TypeExprKind::Named)
          if (const LocalType *Record = Land.recordNamed(Pointee.Name))
            PointerAliases[Record->Name].push_back(Type->Name);
      }
    }

  std::vector<TypeFit> Found;
  for (const LocalType *Type : Library.types()) {
    TypeFit Fit;
    Fit.Type = Type;
    if (isRecordType(*Type)) {
      Fit.Record = Type;
    } else if (Query.RecordsOnly) {
      continue;
    } else if (Type->Kind == TypeKind::Typedef && Type->Target) {
      // A typedef of a record or of a pointer to one is judged by it.
      const TypeExpr &Shape = Land.shapeOf(*Type->Target);
      if (Shape.Kind == TypeExprKind::Named)
        Fit.Record = Land.recordNamed(Shape.Name);
      else if (Shape.Kind == TypeExprKind::Pointer && Shape.Element)
        if (const TypeExpr &Pointee = Land.shapeOf(*Shape.Element);
            Pointee.Kind == TypeExprKind::Named)
          Fit.Record = Land.recordNamed(Pointee.Name);
    }
    if (Fit.Record == Type) {
      if (auto It = Aliases.find(Type->Name); It != Aliases.end())
        Fit.Aliases = It->second;
      if (auto It = PointerAliases.find(Type->Name); It != PointerAliases.end())
        Fit.PointerAliases = It->second;
    }
    if (!TypePart.empty()) {
      std::vector<std::string> Names{Type->Name};
      llvm::append_range(Names, Fit.Aliases);
      llvm::append_range(Names, Fit.PointerAliases);
      auto Match = bestMatch(TypePart, Names);
      if (!Match)
        continue;
      Fit.MatchScore = Match->first.Score;
      Fit.MatchedName = Names[Match->second];
      Fit.MatchedRuns = std::move(Match->first.Runs);
    }
    if (Members) {
      if (!Fit.Record)
        continue;
      std::vector<std::vector<MemberStep>> Paths;
      std::vector<MemberStep> Path;
      Land.memberPaths(*Fit.Record->Record, Path, Paths, 0);
      std::optional<FuzzyMatch> Best;
      for (std::vector<MemberStep> &Candidate : Paths) {
        std::optional<FuzzyMatch> Match =
            fuzzyMatch(MemberPart, memberPathText(Candidate));
        if (Match && (!Best || Match->Score > Best->Score)) {
          Best = std::move(Match);
          Fit.MatchedMember = std::move(Candidate);
        }
      }
      if (!Best)
        continue;
      Fit.MatchScore += Best->Score;
      Fit.MatchedMemberRuns = std::move(Best->Runs);
    }
    if (!Query.Accesses.empty()) {
      if (Fit.Record)
        judge(Library, Land, *Fit.Record->Record, Query, Fit);
      else
        Fit.Outside = static_cast<unsigned>(Query.Accesses.size());
    }
    Found.push_back(std::move(Fit));
  }

  // Room past the last access: the smallest record that holds every access
  // explains them best.
  auto Slack = [&](const TypeFit &Fit) -> uint64_t {
    if (!Fit.Record)
      return std::numeric_limits<uint64_t>::max();
    int64_t End = 0;
    for (const PointerAccess &Access : Query.Accesses)
      End = std::max<int64_t>(End, Access.Offset +
                                       static_cast<int64_t>(Fit.Shift) +
                                       static_cast<int64_t>(Access.Bytes));
    const uint64_t Size = Fit.Record->Record->Size;
    return End >= 0 && static_cast<uint64_t>(End) <= Size
               ? Size - static_cast<uint64_t>(End)
               : std::numeric_limits<uint64_t>::max();
  };
  const bool Judged = !Query.Accesses.empty();
  std::vector<std::pair<uint64_t, std::string>> Keys;
  Keys.reserve(Found.size());
  for (const TypeFit &Fit : Found)
    Keys.emplace_back(Judged ? Slack(Fit) : 0, lowered(Fit.Type->Name));
  std::vector<size_t> Order(Found.size());
  for (size_t I = 0; I < Order.size(); ++I)
    Order[I] = I;
  llvm::stable_sort(Order, [&](size_t A, size_t B) {
    const TypeFit &L = Found[A], &R = Found[B];
    if (Judged) {
      const Counts LC{L.Exact, L.ExactElements, L.Partial, L.Padding,
                      L.Outside};
      const Counts RC{R.Exact, R.ExactElements, R.Partial, R.Padding,
                      R.Outside};
      if (LC.key() != RC.key())
        return LC.key() < RC.key();
      if (L.MatchScore != R.MatchScore)
        return L.MatchScore > R.MatchScore;
      // Of records the accesses fit alike, the program's own describe its
      // build: a system's headers declare thousands of small records, such
      // as ACE_HEADER, that a few accesses fit as tightly by chance.
      const int LO = originRank(L.Type->Origin),
                RO = originRank(R.Type->Origin);
      if (LO != RO)
        return LO < RO;
      if (Keys[A].first != Keys[B].first)
        return Keys[A].first < Keys[B].first;
    } else {
      if (L.MatchScore != R.MatchScore)
        return L.MatchScore > R.MatchScore;
      if (L.Type->Name.size() != R.Type->Name.size())
        return L.Type->Name.size() < R.Type->Name.size();
    }
    const int LO = originRank(L.Type->Origin), RO = originRank(R.Type->Origin);
    if (LO != RO)
      return LO < RO;
    if (Keys[A].second != Keys[B].second)
      return Keys[A].second < Keys[B].second;
    return L.Type->Ordinal < R.Type->Ordinal;
  });
  if (Total)
    *Total = Found.size();
  std::vector<TypeFit> Ranked;
  Ranked.reserve(std::min(Query.Limit, Found.size()));
  for (size_t I = 0; I < Order.size() && Ranked.size() < Query.Limit; ++I)
    Ranked.push_back(std::move(Found[Order[I]]));
  return Ranked;
}

} // namespace neverd::types
