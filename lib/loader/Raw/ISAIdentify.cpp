//===- ISAIdentify.cpp - The instruction set a binary file holds ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Each instruction set the model knows has a table of log P(next byte |
// byte) and one of log P(byte | position in an aligned 4-byte word), both
// quantized to 16 levels.  A window of a file is scored by summing its byte
// pairs' levels under every set; the set that explains the window best by
// more than a margin over every set of another family takes the window's
// vote, weighted by the excess.  Windows of padding, text and compressed
// data never reach the vote: their entropy or printable bytes give them
// away, and the windows left that no set explains clearly are data too.
//
// A family settles the file when it takes nearly all of the weight, and
// enough of it for the file's size: code of a set the model lacks scatters a
// little weight over a few windows.  The family's position statistics then
// tell where its instructions start, and a family of a 32-bit and a 64-bit
// set reads its instructions there: the 64-bit set's code holds encodings
// the 32-bit set lacks, the 32-bit set's none.
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/Raw/ISAIdentify.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"

#include <array>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstring>
#include <numeric>

namespace neverd {
namespace {

/// lib/loader/Raw/ISAModel.bin, as the build embeds it.
constexpr uint8_t ModelBytes[] = {
#include "ISAModel.inc"
};

#define NEVERD_RAW_ISA_LIMIT(Id, Value) constexpr uint64_t k##Id = Value;
#include "neverd/loader/Raw/RawISA.def"

struct ISAInfo {
  llvm::StringLiteral ISA, Name, Processor, Family;
};

constexpr ISAInfo ISAs[] = {
#define NEVERD_RAW_ISA(Id, Name, Processor, Family)                            \
  {Id, Name, Processor, Family},
#include "neverd/loader/Raw/RawISA.def"
};

struct FamilyInfo {
  llvm::StringLiteral Family;
  unsigned Unit;
};

constexpr FamilyInfo Families[] = {
#define NEVERD_RAW_FAMILY(Family, Unit) {Family, Unit},
#include "neverd/loader/Raw/RawISA.def"
};

/// How a family's instructions are read to tell its width.
enum class WidthReader {
  /// 32-bit words in the family's byte order.
  Word,
  /// RISC-V: 16-bit parcels, two to an instruction when its low bits say.
  Parcel,
  /// Every two bytes, the first high.
  BytePair,
};

struct WidthInfo {
  llvm::StringLiteral Family, Narrow, Wide, Encoding;
  WidthReader Reader;
  bool BigEndian;
  llvm::StringLiteral Evidence;
};

constexpr WidthInfo Widths[] = {
#define NEVERD_RAW_WIDTH(Family, Narrow, Wide, Encoding, Reader, BigEndian,    \
                         Evidence)                                             \
  {Family, Narrow, Wide, Encoding, WidthReader::Reader, BigEndian, Evidence},
#include "neverd/loader/Raw/RawISA.def"
};

struct WidePattern {
  llvm::StringLiteral Encoding;
  uint32_t Mask, Value;
};

constexpr WidePattern WidePatterns[] = {
#define NEVERD_RAW_WIDE(Encoding, Mask, Value) {Encoding, Mask, Value},
#include "neverd/loader/Raw/RawISA.def"
};

/// ISAModel.bin: an 8-byte magic, the version, the set count, the level
/// count and the word positions; one 32-byte record per set (its name, then
/// the low and step its pair levels and its position levels dequantize
/// with); then per set its pair table, indexed by byte * 256 + next byte,
/// and its position table, indexed by position * 256 + byte, two levels a
/// byte, low nibble first.
constexpr llvm::StringLiteral Magic = "NDISAMDL";
constexpr size_t HeaderBytes = 24, RecordBytes = 32, NameBytes = 16;
constexpr size_t Pairs = 65536, PairTableBytes = Pairs / 2;
constexpr size_t Positions = 4, PositionCells = Positions * 256,
                 PositionTableBytes = PositionCells / 2;
constexpr uint32_t Version = 2, Levels = 16;

struct Model {
  /// The sets the model holds that RawISA.def describes.
  llvm::SmallVector<const ISAInfo *, 32> Sets;
  llvm::SmallVector<double, 32> Low, Step;
  /// The levels of every set, by byte pair: Levels[Pair * Sets + Set].
  std::vector<uint8_t> Levels;
  /// log P(byte | position) per set, by position * 256 + byte.
  std::vector<std::array<float, PositionCells>> PositionScores;
  /// The x86-style encodings' byte pairs, by encoding.
  llvm::SmallVector<std::pair<llvm::StringRef, std::bitset<Pairs>>, 4> PairSets;
};

uint8_t level(const uint8_t *Table, size_t Index) {
  return Index % 2 ? Table[Index / 2] >> 4 : Table[Index / 2] & 0xf;
}

const Model &model() {
  static const Model Loaded = [] {
    Model M;
    const llvm::ArrayRef<uint8_t> Bytes(ModelBytes);
    using llvm::support::endian::read32le;
    if (Bytes.size() < HeaderBytes ||
        std::memcmp(Bytes.data(), Magic.data(), Magic.size()) != 0 ||
        read32le(Bytes.data() + 8) != Version ||
        read32le(Bytes.data() + 16) != Levels ||
        read32le(Bytes.data() + 20) != Positions)
      return M;
    const uint32_t Count = read32le(Bytes.data() + 12);
    const size_t TableBytes = PairTableBytes + PositionTableBytes;
    if (Bytes.size() != HeaderBytes + Count * (RecordBytes + TableBytes))
      return M;
    const uint8_t *First = Bytes.data() + HeaderBytes + Count * RecordBytes;
    llvm::SmallVector<const uint8_t *, 32> Tables;
    for (uint32_t I = 0; I < Count; ++I) {
      const uint8_t *Record = Bytes.data() + HeaderBytes + I * RecordBytes;
      const llvm::StringRef Name(
          reinterpret_cast<const char *>(Record),
          strnlen(reinterpret_cast<const char *>(Record), NameBytes));
      const auto *Info = llvm::find_if(
          ISAs, [&](const ISAInfo &Known) { return Known.ISA == Name; });
      if (Info == std::end(ISAs))
        continue;
      const auto Float = [&](size_t At) {
        return double(std::bit_cast<float>(read32le(Record + At)));
      };
      M.Sets.push_back(Info);
      M.Low.push_back(Float(NameBytes));
      M.Step.push_back(Float(NameBytes + 4));
      const double PositionLow = Float(NameBytes + 8),
                   PositionStep = Float(NameBytes + 12);
      const uint8_t *Table = First + I * TableBytes;
      Tables.push_back(Table);
      auto &Scores = M.PositionScores.emplace_back();
      for (size_t Cell = 0; Cell < PositionCells; ++Cell)
        Scores[Cell] = float(
            PositionLow + PositionStep * level(Table + PairTableBytes, Cell));
    }
    const size_t Sets = M.Sets.size();
    M.Levels.resize(Pairs * Sets);
    for (size_t Set = 0; Set < Sets; ++Set)
      for (size_t Pair = 0; Pair < Pairs; ++Pair)
        M.Levels[Pair * Sets + Set] = level(Tables[Set], Pair);
    for (const WidthInfo &Width : Widths) {
      if (Width.Reader != WidthReader::BytePair ||
          llvm::any_of(M.PairSets, [&](const auto &Known) {
            return Known.first == Width.Encoding;
          }))
        continue;
      std::bitset<Pairs> Matches;
      for (const WidePattern &Pattern : WidePatterns)
        if (Pattern.Encoding == Width.Encoding)
          for (size_t Pair = 0; Pair < Pairs; ++Pair)
            if ((Pair & Pattern.Mask) == Pattern.Value)
              Matches.set(Pair);
      M.PairSets.push_back({Width.Encoding, Matches});
    }
    return M;
  }();
  return Loaded;
}

struct Vote {
  size_t Set;
  double Weight;
};

struct WindowRead {
  /// Whether the window may be code: not padding, text or compressed.
  bool MayBeCode = false;
  /// Its vote, when one set explains it clearly.
  std::optional<Vote> Cast;
};

WindowRead readWindow(const Model &M, llvm::ArrayRef<uint8_t> Window) {
  std::array<uint32_t, 256> Histogram{};
  for (uint8_t Byte : Window)
    ++Histogram[Byte];
  double Entropy = 0;
  uint64_t Printable = 0;
  for (unsigned Byte = 0; Byte < 256; ++Byte) {
    if (Histogram[Byte] == 0)
      continue;
    const double Share = double(Histogram[Byte]) / Window.size();
    Entropy -= Share * std::log2(Share);
    if ((Byte >= 0x20 && Byte < 0x7f) || Byte == '\t' || Byte == '\n' ||
        Byte == '\r')
      Printable += Histogram[Byte];
  }
  // Padding, compressed or encrypted bytes, and text.
  WindowRead Read;
  if (Entropy * 10 < kLeastEntropyTenths || Entropy * 10 > kMostEntropyTenths ||
      Printable * 100 >= Window.size() * kTextPercent)
    return Read;
  Read.MayBeCode = true;

  const size_t Sets = M.Sets.size();
  llvm::SmallVector<uint32_t, 32> Sums(Sets, 0);
  for (size_t I = 1; I < Window.size(); ++I) {
    const uint8_t *Row =
        &M.Levels[(size_t(Window[I - 1]) << 8 | Window[I]) * Sets];
    for (size_t Set = 0; Set < Sets; ++Set)
      Sums[Set] += Row[Set];
  }
  // Log likelihood per byte pair under each set.
  const double PairsRead = double(Window.size() - 1);
  llvm::SmallVector<double, 32> Scores(Sets);
  for (size_t Set = 0; Set < Sets; ++Set)
    Scores[Set] = M.Low[Set] + M.Step[Set] * Sums[Set] / PairsRead;
  const size_t Top =
      std::max_element(Scores.begin(), Scores.end()) - Scores.begin();
  double Other = -INFINITY;
  for (size_t Set = 0; Set < Sets; ++Set)
    if (M.Sets[Set]->Family != M.Sets[Top]->Family)
      Other = std::max(Other, Scores[Set]);
  const double Margin = Scores[Top] - Other, Least = kMarginMilli / 1000.0;
  if (Margin >= Least)
    Read.Cast = Vote{Top, Margin - Least};
  return Read;
}

/// Where instructions of \p Set start in \p Windows, modulo \p Unit: the
/// rotation under which the bytes fit the set's position statistics best,
/// when it fits clearly better than every rotation of another start; the
/// file's own alignment otherwise.
unsigned readCodeOffset(const Model &M, size_t Set,
                        llvm::ArrayRef<llvm::ArrayRef<uint8_t>> Windows,
                        unsigned Unit) {
  const auto &Scores = M.PositionScores[Set];
  std::array<double, Positions> Fit{};
  size_t Bytes = 0;
  for (llvm::ArrayRef<uint8_t> Window : Windows) {
    Bytes += Window.size();
    for (size_t I = 0; I < Window.size(); ++I)
      for (size_t Rotation = 0; Rotation < Positions; ++Rotation)
        Fit[Rotation] += Scores[(I - Rotation) % Positions * 256 + Window[I]];
  }
  const size_t Best = std::max_element(Fit.begin(), Fit.end()) - Fit.begin();
  double Other = -INFINITY;
  for (size_t Rotation = 0; Rotation < Positions; ++Rotation)
    if (Rotation % Unit != Best % Unit)
      Other = std::max(Other, Fit[Rotation]);
  if (!Bytes || (Fit[Best] - Other) * 1000 < double(kOffsetMilli) * Bytes)
    return 0;
  return unsigned(Best % Unit);
}

/// Of the instructions in \p Window from \p Offset on, the share only the
/// 64-bit set of \p Width has.
double readWideShare(const Model &M, const WidthInfo &Width,
                     llvm::ArrayRef<uint8_t> Window, unsigned Offset) {
  using namespace llvm::support::endian;
  llvm::SmallVector<const WidePattern *, 32> Patterns;
  for (const WidePattern &Pattern : WidePatterns)
    if (Pattern.Encoding == Width.Encoding)
      Patterns.push_back(&Pattern);
  const auto IsWide = [&](uint32_t Instruction) {
    return llvm::any_of(Patterns, [&](const WidePattern *Pattern) {
      return (Instruction & Pattern->Mask) == Pattern->Value;
    });
  };
  size_t Read = 0, Wide = 0;
  switch (Width.Reader) {
  case WidthReader::Word:
    for (size_t I = Offset % 4; I + 4 <= Window.size(); I += 4, ++Read)
      Wide +=
          IsWide(Width.BigEndian ? read32be(&Window[I]) : read32le(&Window[I]));
    break;
  case WidthReader::Parcel:
    for (size_t I = Offset % 2; I + 2 <= Window.size(); ++Read) {
      if ((Window[I] & 3) != 3) {
        I += 2;
        continue;
      }
      if (I + 4 > Window.size())
        break;
      Wide += IsWide(read32le(&Window[I]));
      I += 4;
    }
    break;
  case WidthReader::BytePair: {
    const auto *Pairs = llvm::find_if(M.PairSets, [&](const auto &Known) {
      return Known.first == Width.Encoding;
    });
    for (size_t I = 1; I < Window.size(); ++I, ++Read)
      Wide += Pairs->second.test(size_t(Window[I - 1]) << 8 | Window[I]);
    break;
  }
  }
  return Read ? double(Wide) / Read : 0;
}

ISAGuess guessOf(const ISAInfo &Info, double Share) {
  return {Info.ISA, Info.Name, Info.Processor, Share};
}

} // namespace

ISAIdentification identifyISA(llvm::ArrayRef<uint8_t> Bytes) {
  using Verdict = ISAIdentification::Verdict;
  ISAIdentification Result;
  Result.Fingerprint = readRawFingerprint(Bytes);
  const Model &M = model();
  if (M.Sets.empty() || Bytes.size() < kLeastBytes)
    return Result;
  // A file smaller than a window is one window.
  const size_t Window = std::min<size_t>(kWindowBytes, Bytes.size());
  const size_t Windows = Bytes.size() / Window;
  const size_t Read = std::min<size_t>(Windows, kMostWindows);
  struct Cast {
    llvm::ArrayRef<uint8_t> Window;
    Vote Choice;
  };
  llvm::SmallVector<Cast, 0> Casts;
  size_t MayBeCode = 0;
  for (size_t I = 0; I < Read; ++I) {
    // A large file's windows are read evenly spread over it.
    const size_t Index = Read == Windows ? I : I * Windows / Read;
    const llvm::ArrayRef<uint8_t> Slice = Bytes.slice(Index * Window, Window);
    const WindowRead W = readWindow(M, Slice);
    MayBeCode += W.MayBeCode;
    if (W.Cast)
      Casts.push_back({Slice, *W.Cast});
  }
  Result.CodeShare = Read ? double(Casts.size()) / Read : 0;
  if (Casts.empty())
    return Result;

  // The weight and the windows each family and each set took.
  llvm::SmallVector<llvm::StringRef, 32> FamilyOf;
  for (const ISAInfo *Info : M.Sets)
    FamilyOf.push_back(Info->Family);
  llvm::SmallVector<double, 32> SetWeight(M.Sets.size(), 0);
  llvm::StringMap<double> FamilyWeight;
  llvm::StringMap<size_t> FamilyVotes;
  double Total = 0;
  for (const Cast &C : Casts) {
    SetWeight[C.Choice.Set] += C.Choice.Weight;
    FamilyWeight[FamilyOf[C.Choice.Set]] += C.Choice.Weight;
    ++FamilyVotes[FamilyOf[C.Choice.Set]];
    Total += C.Choice.Weight;
  }
  // Each family as its likeliest set, most weight first.
  llvm::SmallVector<size_t, 32> Leaders;
  for (size_t Set = 0; Set < M.Sets.size(); ++Set) {
    if (SetWeight[Set] <= 0)
      continue;
    auto *Known = llvm::find_if(Leaders, [&](size_t Other) {
      return FamilyOf[Other] == FamilyOf[Set];
    });
    if (Known == Leaders.end())
      Leaders.push_back(Set);
    else if (SetWeight[Set] > SetWeight[*Known])
      *Known = Set;
  }
  llvm::stable_sort(Leaders, [&](size_t A, size_t B) {
    return FamilyWeight[FamilyOf[A]] > FamilyWeight[FamilyOf[B]];
  });
  const size_t Top = Leaders.front();
  const llvm::StringRef Family = FamilyOf[Top];
  const double Share = Total > 0 ? FamilyWeight[Family] / Total : 0;
  const size_t Votes = FamilyVotes[Family];
  // Nearly all of the weight, from enough windows -- every window of a file
  // with fewer -- with enough weight for the windows that may be code, in
  // all, and per window that voted.
  const double Weight = FamilyWeight[Family] * 1000;
  const bool Settled = Share * 100 >= kFamilyPercent &&
                       Votes >= std::min<size_t>(kLeastVotes, MayBeCode) &&
                       Weight >= double(kWeightMilliPerWindow) * MayBeCode &&
                       Weight >= kLeastWeightMilli &&
                       Weight >= double(kVoteWeightMilli) * Votes;

  // The leading family's windows, spread evenly, tell where its
  // instructions start and, for a family of two widths, which.
  llvm::SmallVector<llvm::ArrayRef<uint8_t>, 0> Own;
  for (const Cast &C : Casts)
    if (FamilyOf[C.Choice.Set] == Family)
      Own.push_back(C.Window);
  if (Own.size() > kMostWidthWindows) {
    llvm::SmallVector<llvm::ArrayRef<uint8_t>, 0> Spread;
    for (size_t I = 0; I < kMostWidthWindows; ++I)
      Spread.push_back(Own[I * Own.size() / kMostWidthWindows]);
    Own = std::move(Spread);
  }
  const auto *Unit = llvm::find_if(
      Families, [&](const FamilyInfo &Info) { return Info.Family == Family; });
  if (Unit != std::end(Families) && Unit->Unit > 1) {
    Result.CodeUnit = Unit->Unit;
    Result.CodeOffset = readCodeOffset(M, Top, Own, Unit->Unit);
  }
  const auto *Width = llvm::find_if(
      Widths, [&](const WidthInfo &Info) { return Info.Family == Family; });
  const ISAInfo *Resolved = M.Sets[Top];
  if (Width != std::end(Widths)) {
    llvm::SmallVector<double, 0> Shares;
    for (llvm::ArrayRef<uint8_t> Window : Own)
      Shares.push_back(readWideShare(M, *Width, Window, Result.CodeOffset));
    std::nth_element(Shares.begin(), Shares.begin() + Shares.size() / 2,
                     Shares.end());
    Result.WideShare = Shares[Shares.size() / 2];
    const auto Named = [&](llvm::StringRef ISA) {
      return llvm::find_if(
          ISAs, [&](const ISAInfo &Info) { return Info.ISA == ISA; });
    };
    if (*Result.WideShare * 10000 <= kNarrowMostBasisPoints)
      Resolved = Named(Width->Narrow);
    else if (*Result.WideShare * 10000 >= kWideLeastBasisPoints)
      Resolved = Named(Width->Wide);
    else
      Resolved = nullptr;
  }

  if (!Settled)
    Result.Outcome = Verdict::Unclear;
  else
    Result.Outcome = Resolved ? Verdict::Settled : Verdict::WidthUnclear;
  for (size_t Set : Leaders) {
    const double Listed = FamilyWeight[FamilyOf[Set]] / Total;
    if (Listed * 100 < kListedPercent)
      break;
    if (Set != Top) {
      Result.Guesses.push_back(guessOf(*M.Sets[Set], Listed));
      continue;
    }
    if (Resolved) {
      Result.Guesses.push_back(guessOf(*Resolved, Listed));
      continue;
    }
    // Both widths, split as the windows voted.
    llvm::SmallVector<size_t, 2> Members;
    for (size_t Member = 0; Member < M.Sets.size(); ++Member)
      if (FamilyOf[Member] == Family)
        Members.push_back(Member);
    llvm::stable_sort(Members, [&](size_t A, size_t B) {
      return SetWeight[A] > SetWeight[B];
    });
    const double FamilyTotal = FamilyWeight[Family];
    for (size_t Member : Members)
      Result.Guesses.push_back(
          guessOf(*M.Sets[Member], Listed * SetWeight[Member] / FamilyTotal));
  }
  return Result;
}

std::optional<double> readWideShare(llvm::StringRef Family,
                                    llvm::ArrayRef<uint8_t> Code,
                                    unsigned Offset) {
  const auto *Width = llvm::find_if(
      Widths, [&](const WidthInfo &Info) { return Info.Family == Family; });
  if (Width == std::end(Widths))
    return std::nullopt;
  return readWideShare(model(), *Width, Code, Offset);
}

llvm::StringRef ISAIdentification::outcomeName() const {
  switch (Outcome) {
#define NEVERD_RAW_VERDICT(Id, Name)                                           \
  case Verdict::Id:                                                            \
    return Name;
#include "neverd/loader/Raw/RawISAVerdict.def"
  }
  llvm_unreachable("every verdict is named");
}

llvm::StringRef ISAIdentification::detectedProcessor() const {
  if (Fingerprint)
    return Fingerprint->Processor;
  if (Outcome == Verdict::Settled && !Guesses.empty())
    return Guesses.front().Processor;
  return {};
}

std::string ISAIdentification::describe() const {
  if (Fingerprint)
    return llvm::formatv("a {0} at the start of the file", Fingerprint->Name)
        .str();
  if (Guesses.empty())
    return "no part of the file looks like code of a set NeverD knows";
  const auto Percent = [](double Share) {
    return static_cast<int>(std::lround(Share * 100));
  };
  std::string Text;
  if (Outcome == Verdict::WidthUnclear) {
    Text = llvm::formatv("{0} or {1}, {2}% of the code together; its "
                         "instructions do not tell which",
                         Guesses[0].Name, Guesses[1].Name,
                         Percent(Guesses[0].Share + Guesses[1].Share))
               .str();
  } else {
    for (const ISAGuess &Guess : llvm::ArrayRef(Guesses).take_front(3)) {
      if (!Text.empty())
        Text += ", ";
      Text += llvm::formatv("{0} {1}%", Guess.Name, Percent(Guess.Share)).str();
    }
    Text += " of the code";
  }
  // What told the width: the 64-bit set's encodings, or their absence.
  const auto *Info = llvm::find_if(ISAs, [&](const ISAInfo &Known) {
    return Known.ISA == Guesses.front().ISA;
  });
  const auto *Width = Info == std::end(ISAs)
                          ? std::end(Widths)
                          : llvm::find_if(Widths, [&](const WidthInfo &W) {
                              return W.Family == Info->Family;
                            });
  if (Outcome == Verdict::Settled && WideShare && Width != std::end(Widths)) {
    if (*WideShare == 0)
      Text += llvm::formatv(", no {0}", Width->Evidence).str();
    else if (Guesses.front().ISA == Width->Wide)
      Text += llvm::formatv(", {0} in {1:f1}% of it", Width->Evidence,
                            *WideShare * 100)
                  .str();
    else
      Text += llvm::formatv(", {0} in only {1:f2}% of it", Width->Evidence,
                            *WideShare * 100)
                  .str();
  }
  Text +=
      llvm::formatv("; {0}% of the file looks like code", Percent(CodeShare))
          .str();
  if (CodeOffset)
    Text +=
        llvm::formatv("; its instructions align to {0} bytes from offset {1}",
                      CodeUnit, CodeOffset)
            .str();
  return Text;
}

} // namespace neverd
