//===- TextEvidence.cpp - Whether decoded characters are text -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "TextEvidence.h"

#include "neverd/support/StringScan.h"

#include <algorithm>
#include <vector>

namespace neverd::strings {
namespace detail {
namespace {

#include "CommonCharacters.inc"

bool isIdeograph(uint32_t C) {
  return (C >= 0x3400 && C <= 0x9fff) || (C >= 0xac00 && C <= 0xd7a3);
}

/// An ideograph of the first level of a national standard (GB2312, Big5,
/// JIS X 0208) or a KS X 1001 Hangul syllable.
bool isCommonIdeograph(uint32_t C) {
  if (C >= 0x3400 && C <= 0x9fff)
    return CommonHan[(C - 0x3400) >> 6] >> ((C - 0x3400) & 63) & 1;
  if (C >= 0xac00 && C <= 0xd7a3)
    return CommonHangul[(C - 0xac00) >> 6] >> ((C - 0xac00) & 63) & 1;
  return false;
}

/// Strings with at most this many ideographs need all of them common.
constexpr unsigned ShortIdeographicText = 10;
/// From this many characters on, text beyond ASCII may not repeat one
/// character in two in five positions.
constexpr size_t TablePatternLength = 6;

/// The writing system of a letter beyond ASCII (StringEncodings.def).
enum class Family : uint8_t {
  None,
  Latin,
  Greek,
  Cyrillic,
  Hebrew,
  Arabic,
  Indic,
  Thai,
  Japanese,
  Chinese,
  Han,
  Korean
};

Family familyOf(uint32_t C) {
#define NEVERD_SCRIPT_FAMILY(First, Last, Kind)                                \
  if (C >= First && C <= Last)                                                 \
    return Family::Kind;
#include "neverd/support/StringEncodings.def"
  return Family::None;
}

bool isASCIILetter(uint32_t C) {
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z');
}

bool isEastAsian(Family Kind) {
  return Kind == Family::Han || Kind == Family::Japanese ||
         Kind == Family::Chinese || Kind == Family::Korean;
}

/// The case of a letter: 1 uppercase, -1 lowercase, 0 none or unknown.
int letterCase(uint32_t C) {
  if ((C >= 'A' && C <= 'Z') || (C >= 0xc0 && C <= 0xde && C != 0xd7) ||
      (C >= 0x391 && C <= 0x3a9) || (C >= 0x400 && C <= 0x42f))
    return 1;
  if ((C >= 'a' && C <= 'z') || (C >= 0xdf && C <= 0xff && C != 0xf7) ||
      (C >= 0x3b1 && C <= 0x3c9) || (C >= 0x430 && C <= 0x45f))
    return -1;
  // Latin Extended-A pairs each capital with the small letter after it.
  if (C >= 0x100 && C <= 0x17f)
    return C & 1 ? -1 : 1;
  return 0;
}

} // namespace

WideTextSet::WideTextSet() {
  for (uint32_t C = 0; C < 0x80; ++C)
    if (isTextASCII(C))
      set(C);
#define NEVERD_TEXT_BLOCK(First, Last)                                         \
  for (uint32_t C = First; C <= Last && C <= 0xffff; ++C)                      \
    if (isPrintableBeyondASCII(C))                                             \
      set(C);
#include "neverd/support/StringEncodings.def"
}

bool WideTextSet::containsSupplementary(uint32_t C) const {
#define NEVERD_TEXT_BLOCK(First, Last)                                         \
  if (First > 0xffff && C >= First && C <= Last)                               \
    return isPrintableBeyondASCII(C);
#include "neverd/support/StringEncodings.def"
  return false;
}

const WideTextSet &wideText() {
  static const WideTextSet Set;
  return Set;
}

bool isLegacyLetter(uint32_t C) {
#define NEVERD_LEGACY_LETTERS(First, Last)                                     \
  if (C >= First && C <= Last)                                                 \
    return true;
#include "neverd/support/StringEncodings.def"
  return false;
}

bool readsAsText(llvm::ArrayRef<uint32_t> Codes, Source From) {
  Family Script = Family::None;
  bool Han = false;
  unsigned ASCIILetters = 0, ASCIIAlnum = 0, Letters = 0, Inside = 0;
  unsigned EastAsian = 0, Ideographs = 0, Common = 0;
  std::vector<uint32_t> Beyond;
  // The current word: whether it holds ASCII letters and letters beyond,
  // and the cases of its letters so far.
  bool WordASCII = false, WordBeyond = false, CaseValid = true;
  unsigned WordLetters = 0;
  int FirstCase = 0, RestCase = 0;
  for (size_t I = 0; I <= Codes.size(); ++I) {
    const uint32_t C = I < Codes.size() ? Codes[I] : 0;
    const Family Kind = C >= 0x80 ? familyOf(C) : Family::None;
    const bool ASCIILetter = isASCIILetter(C);
    if (!ASCIILetter && Kind == Family::None) {
      // A word ends.
      if (WordASCII && WordBeyond && Script != Family::Latin)
        return false;
      if (WordBeyond && !CaseValid)
        return false;
      WordASCII = WordBeyond = false;
      CaseValid = true;
      WordLetters = 0;
      FirstCase = RestCase = 0;
    } else if (const int Case = letterCase(C)) {
      // Lowercase, Capitalized or uppercase: after the first letter, every
      // letter has one case, lowercase unless the first is uppercase.
      if (WordLetters++ == 0)
        FirstCase = Case;
      else if (RestCase && Case != RestCase)
        CaseValid = false;
      else if (!RestCase && FirstCase == -1 && Case == 1)
        CaseValid = false;
      else
        RestCase = Case;
    }
    if (I == Codes.size())
      break;
    if (C < 0x80) {
      ASCIILetters += ASCIILetter;
      ASCIIAlnum += ASCIILetter || (C >= '0' && C <= '9');
      WordASCII |= ASCIILetter;
      continue;
    }
    Beyond.push_back(C);
    if (isIdeograph(C)) {
      ++Ideographs;
      Common += isCommonIdeograph(C);
    }
    if (Kind == Family::None)
      continue;
    WordBeyond = true;
    ++Letters;
    EastAsian += isEastAsian(Kind);
    const auto isLetter = [&](size_t At) {
      return At < Codes.size() &&
             (isASCIILetter(Codes[At]) || familyOf(Codes[At]) != Family::None);
    };
    Inside += isLetter(I + 1) || (I > 0 && isLetter(I - 1));
    if (Kind == Family::Han) {
      Han = true;
      continue;
    }
    if (Script != Family::None && Script != Kind)
      return false;
    Script = Kind;
  }
  if (Han && Script != Family::None && !isEastAsian(Script))
    return false;
  const auto longestRun = [](std::vector<uint32_t> Values) {
    std::sort(Values.begin(), Values.end());
    size_t Longest = 0;
    for (size_t I = 0, Run = 0; I < Values.size(); ++I) {
      Run = I && Values[I] == Values[I - 1] ? Run + 1 : 1;
      Longest = std::max(Longest, Run);
    }
    return Longest;
  };
  if (Beyond.size() >= 2) {
    const size_t Longest = longestRun(Beyond);
    if (Longest * 2 > Beyond.size() && (Beyond.size() >= 3 || Longest == 2))
      return false;
    if (Codes.size() >= TablePatternLength &&
        longestRun(Codes) * 5 >= Codes.size() * 2)
      return false;
  }
  if (Ideographs <= ShortIdeographicText ? Common < Ideographs
                                         : Common * 10 < Ideographs * 9)
    return false;
  if (EastAsian || From == Source::DoubleByte)
    return EastAsian >= 2;
  if (From == Source::Wide && !Letters)
    return ASCIIAlnum >= 2;
  const unsigned AllLetters = ASCIILetters + Letters;
  const unsigned Needed =
      (Script == Family::Latin || !Letters) && From != Source::SingleByte ? 3
                                                                          : 4;
  return AllLetters >= Needed && Inside * 5 >= Letters * 4 &&
         (Script != Family::Latin || Letters * 5 <= AllLetters * 3) &&
         (From != Source::SingleByte || Letters);
}

} // namespace detail

unsigned displayColumns(uint32_t C) {
#define NEVERD_WIDE_CHARACTERS(First, Last)                                    \
  if (C >= (First) && C <= (Last))                                             \
    return 2;
#include "neverd/support/WideCharacters.def"
  return 1;
}

bool isShownCharacter(uint32_t C) {
  return C >= 0x20 && C != 0x7f && !(C >= 0x80 && C < 0xa0) &&
         !(C >= 0x200b && C <= 0x200f) && !(C >= 0x202a && C <= 0x202e) &&
         !(C >= 0x2060 && C <= 0x206f) && C != 0xfeff &&
         !(C >= 0xd800 && C <= 0xf8ff) && C < 0xf0000;
}

} // namespace neverd::strings
