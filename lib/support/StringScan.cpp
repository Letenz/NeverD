//===- StringScan.cpp - text strings in binary data ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The scanners: where a string may start and end in each encoding.  How
// bytes decode is TextCodec's, and whether the characters are text is
// TextEvidence's.
//
//===----------------------------------------------------------------------===//

#include "neverd/support/StringScan.h"

#include "TextCodec.h"
#include "TextEvidence.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <vector>

namespace neverd::strings {

using namespace detail;

namespace {

/// A C string in \p Legacy from \p Start: its terminator, or nothing when
/// it holds a character that is not text or does not end.  Collects the
/// code points.
const uint8_t *legacyString(const EncodingInfo &Legacy, const uint8_t *Start,
                            const uint8_t *End, std::vector<uint32_t> &Codes) {
  const bool SingleByte = Legacy.Kind == Decoder::SingleByte;
  for (const uint8_t *Cursor = Start; Cursor < End;) {
    if (!*Cursor)
      return Cursor;
    // 0xFFFF marks data, not text, though 0xFF is a letter in some pages.
    if (SingleByte && *Cursor == 0xff && Cursor + 1 < End && Cursor[1] == 0xff)
      return nullptr;
    // GBK alone reads byte 0x80 as the euro sign, which its text seldom
    // writes and data often holds.
    if (Legacy.Kind == Decoder::GB18030 && *Cursor == 0x80)
      return nullptr;
    uint32_t Code[2];
    unsigned Count = 0;
    const unsigned Size = decodeLegacy(Legacy, Cursor, End, Code, Count);
    if (!Size)
      return nullptr;
    for (unsigned I = 0; I < Count; ++I) {
      const uint32_t C = Code[I];
      // Halfwidth katakana come from single bytes 0xA1-0xDF, a quarter of
      // all bytes, and modern text does not use them.
      const bool Text = C < 0x80 ? isTextASCII(C)
                                 : isPrintableBeyondASCII(C) &&
                                       (!SingleByte || isLegacyLetter(C)) &&
                                       !(C >= 0xff61 && C <= 0xff9f);
      if (!Text)
        return nullptr;
      Codes.push_back(C);
    }
    Cursor += Size;
  }
  return nullptr;
}

/// The display columns \p Codes take.
unsigned columnsOf(llvm::ArrayRef<uint32_t> Codes) {
  unsigned Columns = 0;
  for (uint32_t C : Codes)
    Columns += displayColumns(C);
  return Columns;
}

/// A code page, as code pages read a C string.
struct CodePage {
  Encoding Kind;
  const EncodingInfo *Info;
};

/// Reads the C string at \p Start in \p Page into \p Codes: its
/// terminator when it reads as text at least \p MinLength columns wide and
/// not all ASCII, else nothing.  A single-byte page nobody chose (not
/// \p Chosen) needs more evidence.
const uint8_t *readCString(const CodePage &Page, const uint8_t *Start,
                           const uint8_t *End, unsigned MinLength, bool Chosen,
                           std::vector<uint32_t> &Codes) {
  Codes.clear();
  const uint8_t *Terminator = legacyString(*Page.Info, Start, End, Codes);
  const bool SingleByte = Page.Info->Kind == Decoder::SingleByte;
  if (!Terminator || columnsOf(Codes) < MinLength ||
      std::all_of(Codes.begin(), Codes.end(),
                  [](uint32_t C) { return C < 0x80; }) ||
      !longEnoughText(Codes) ||
      !readsAsText(Codes,
                   SingleByte ? Source::SingleByte : Source::DoubleByte) ||
      (SingleByte && !Chosen && !readsAsUnchosenText(Codes)))
    return nullptr;
  return Terminator;
}

/// Whether a code page made for another writing system than \p Chosen's
/// reads the C string at \p Start as text in its own scripts, every page
/// counted whether searched or not and on the evidence a chosen page needs;
/// with \p Foreign, \p Chosen read it in a script it is not made for, and
/// any page's own reading contests it.
bool contested(llvm::ArrayRef<CodePage> Every, Encoding Chosen, bool Foreign,
               const uint8_t *Start, const uint8_t *End, unsigned MinLength) {
  std::vector<uint32_t> Codes;
  for (const CodePage &Page : Every)
    if (Page.Kind != Chosen && (Foreign || !shareScripts(Page.Kind, Chosen)) &&
        readCString(Page, Start, End, MinLength, /*Chosen=*/true, Codes) &&
        inCodePageScripts(Codes, Page.Kind))
      return true;
  return false;
}

/// ASCII and UTF-8 strings ending in NUL, and where a run after a NUL is not
/// UTF-8, a string in one of \p CodePages that reads it as text: the
/// preferred one when \p Preferred names it first; else the first that reads
/// it in the scripts it is made for, or failing that the first that reads it
/// at all, unless a page of \p Every made for another writing system reads
/// it in its own (contested).
void scanCStrings(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
                  llvm::ArrayRef<CodePage> CodePages, bool Preferred,
                  llvm::ArrayRef<CodePage> Every,
                  std::vector<FoundString> &Out) {
  const bool ASCII = Options.Encodings & encodingBit(Encoding::ASCII);
  const bool UTF8 = Options.Encodings & encodingBit(Encoding::UTF8);
  if (!ASCII && !UTF8 && CodePages.empty())
    return;
  const uint8_t *Begin = Data.data();
  const uint8_t *End = Begin + Data.size();
  std::vector<uint32_t> Codes, Kept;
  for (const uint8_t *Cursor = Begin; Cursor < End; ++Cursor) {
    const uint8_t *Start = Cursor;
    unsigned Chars = 0, Columns = 0;
    bool Multibyte = false;
    while (Cursor < End && *Cursor) {
      if (isTextASCII(*Cursor)) {
        ++Chars;
        ++Columns;
        ++Cursor;
        continue;
      }
      uint32_t Code = 0;
      const unsigned Size = UTF8 ? decodeUTF8(Cursor, End, Code) : 0;
      if (!Size || !isPrintableBeyondASCII(Code))
        break;
      ++Chars;
      Columns += displayColumns(Code);
      Cursor += Size;
      Multibyte = true;
    }
    const bool Terminated = Cursor < End && !*Cursor;
    if (Terminated) {
      if (Columns >= Options.MinLength && (Multibyte ? UTF8 : ASCII) &&
          (!Multibyte || [&] {
            const auto Codes = utf8Codes(Start, Cursor);
            return longEnoughText(Codes) && readsAsText(Codes, Source::UTF8);
          }())) {
        FoundString Found;
        Found.Offset = static_cast<uint64_t>(Start - Begin);
        Found.Bytes = static_cast<uint64_t>(Cursor - Start);
        Found.Chars = Chars;
        Found.Kind = Multibyte ? Encoding::UTF8 : Encoding::ASCII;
        Found.Text.assign(reinterpret_cast<const char *>(Start), Found.Bytes);
        Out.push_back(std::move(Found));
      }
      continue;
    }
    // A run UTF-8 does not read may be text in a legacy code page, where a
    // pool of strings would hold one: after a NUL.  Random bytes decode in a
    // double-byte code page most of the time, so starting anywhere else
    // would read most data again from every byte.
    if (CodePages.empty() || Cursor == End || (Start != Begin && Start[-1]))
      continue;
    std::optional<size_t> Chosen;
    bool Foreign = false;
    const uint8_t *Terminator = nullptr;
    for (size_t I = 0; I < CodePages.size(); ++I) {
      const uint8_t *Read =
          readCString(CodePages[I], Start, End, Options.MinLength,
                      Preferred && I == 0, Codes);
      if (!Read)
        continue;
      const bool Own =
          (Preferred && I == 0) || inCodePageScripts(Codes, CodePages[I].Kind);
      if (!Own && Chosen)
        continue;
      Chosen = I;
      Foreign = !Own;
      Terminator = Read;
      Kept.swap(Codes);
      if (Own)
        break;
    }
    if (!Chosen || (!(Preferred && *Chosen == 0) &&
                    contested(Every, CodePages[*Chosen].Kind, Foreign, Start,
                              End, Options.MinLength)))
      continue;
    FoundString Found;
    Found.Offset = static_cast<uint64_t>(Start - Begin);
    Found.Bytes = static_cast<uint64_t>(Terminator - Start);
    Found.Chars = static_cast<unsigned>(Kept.size());
    Found.Kind = CodePages[*Chosen].Kind;
    appendUTF8(Found.Text, Kept);
    Out.push_back(std::move(Found));
    Cursor = Terminator;
  }
}

/// UTF-16 or UTF-32 strings ending in a zero unit, at multiples of the unit
/// size, that read as text (readsAsText).  Random 16-bit data decodes into
/// the large ideograph blocks a third of the time, so a string mostly beyond
/// ASCII also needs a zero unit before it, as in a pool of strings, and must
/// not be ASCII read one byte off (every unit's low byte zero).
template <unsigned Unit, bool BigEndian>
void scanWideStrings(llvm::ArrayRef<uint8_t> Data, Encoding Kind,
                     unsigned MinLength, std::vector<FoundString> &Out) {
  const WideTextSet &Text = wideText();
  const uint8_t *Begin = Data.data();
  const uint8_t *End = Begin + Data.size() / Unit * Unit;
  for (const uint8_t *Cursor = Begin; Cursor < End; Cursor += Unit) {
    const uint8_t *Start = Cursor;
    unsigned Chars = 0, Columns = 0, ASCII = 0;
    bool LowBytesZero = true;
    uint32_t Code = 0;
    // Counted first; the text is decoded only for a candidate that ends.
    while (Cursor < End) {
      const unsigned Used = decodeWide<Unit, BigEndian>(Cursor, End, Code);
      if (!Used || !Text.contains(Code))
        break;
      ++Chars;
      Columns += displayColumns(Code);
      ASCII += Code < 0x80;
      LowBytesZero &= Code < 0x80 || (Code & 0xff) == 0;
      Cursor += Used;
    }
    if (Cursor >= End || readUnit<Unit, BigEndian>(Cursor) ||
        Columns < MinLength)
      continue;
    // Text mostly beyond ASCII also sits where a pool of strings puts it,
    // after a zero unit, and is not ASCII read one byte off.
    if (ASCII * 2 < Chars &&
        (LowBytesZero ||
         (Start != Begin && readUnit<Unit, BigEndian>(Start - Unit))))
      continue;
    std::vector<uint32_t> Codes;
    Codes.reserve(Chars);
    for (const uint8_t *At = Start; At < Cursor;) {
      At += decodeWide<Unit, BigEndian>(At, End, Code);
      Codes.push_back(Code);
    }
    if (!longEnoughText(Codes) || !readsAsText(Codes, Source::Wide))
      continue;
    FoundString Found;
    Found.Offset = static_cast<uint64_t>(Start - Begin);
    Found.Bytes = static_cast<uint64_t>(Cursor - Start);
    Found.Chars = Chars;
    Found.Kind = Kind;
    appendUTF8(Found.Text, Codes);
    Out.push_back(std::move(Found));
  }
}

} // namespace

void scan(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
          llvm::function_ref<void(FoundString &&)> Found) {
  const unsigned MinLength = std::max(1u, Options.MinLength);
  std::vector<FoundString> All;
  ScanOptions Effective = Options;
  Effective.MinLength = MinLength;
  // The code pages searched: the preferred one, then table order.
  std::vector<CodePage> CodePages;
  const auto Known = encodings();
  const bool Preferred =
      Options.Preferred && isLegacyEncoding(*Options.Preferred);
  if (Preferred)
    CodePages.push_back({*Options.Preferred,
                         &Known[static_cast<unsigned>(*Options.Preferred)]});
  std::vector<CodePage> Every;
  for (unsigned I = 0; I < Known.size(); ++I) {
    const auto Kind = static_cast<Encoding>(I);
    if (Known[I].Kind == Decoder::Unicode)
      continue;
    Every.push_back({Kind, &Known[I]});
    if ((Options.Encodings & encodingBit(Kind)) && Kind != Options.Preferred)
      CodePages.push_back({Kind, &Known[I]});
  }
  scanCStrings(Data, Effective, CodePages, Preferred, Every, All);
  const auto searched = [&](Encoding E) {
    return (Options.Encodings & encodingBit(E)) != 0;
  };
  if (searched(Encoding::UTF16LE))
    scanWideStrings<2, false>(Data, Encoding::UTF16LE, MinLength, All);
  if (searched(Encoding::UTF16BE))
    scanWideStrings<2, true>(Data, Encoding::UTF16BE, MinLength, All);
  if (searched(Encoding::UTF32LE))
    scanWideStrings<4, false>(Data, Encoding::UTF32LE, MinLength, All);
  if (searched(Encoding::UTF32BE))
    scanWideStrings<4, true>(Data, Encoding::UTF32BE, MinLength, All);
  // The first string to start keeps the bytes; at one offset, the longer,
  // then the Unicode encoding.
  std::sort(All.begin(), All.end(),
            [](const FoundString &A, const FoundString &B) {
              if (A.Offset != B.Offset)
                return A.Offset < B.Offset;
              if (A.Bytes != B.Bytes)
                return A.Bytes > B.Bytes;
              return A.Kind < B.Kind;
            });
  uint64_t Free = 0;
  for (auto &String : All) {
    if (String.Offset < Free)
      continue;
    Free = String.Offset + String.Bytes + info(String.Kind).UnitBytes;
    Found(std::move(String));
  }
}

} // namespace neverd::strings
