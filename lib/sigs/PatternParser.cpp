//===- PatternParser.cpp - FLIRT .pat text format parser -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/PatternParser.h"

#include "neverd/support/Parallel.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

using namespace neverd::sigs;

namespace {

/// Each character's value as a hex digit, or 0xFF when it is not one.
constexpr std::array<uint8_t, 256> makeHexDigits() {
  std::array<uint8_t, 256> Digits{};
  for (uint8_t &Digit : Digits)
    Digit = 0xFF;
  for (unsigned C = '0'; C <= '9'; ++C)
    Digits[C] = static_cast<uint8_t>(C - '0');
  for (unsigned C = 'a'; C <= 'f'; ++C)
    Digits[C] = static_cast<uint8_t>(C - 'a' + 10);
  for (unsigned C = 'A'; C <= 'F'; ++C)
    Digits[C] = static_cast<uint8_t>(C - 'A' + 10);
  return Digits;
}

constexpr auto HexDigits = makeHexDigits();

/// The characters that separate fields: the whitespace llvm::SplitString and
/// StringRef::trim know.
constexpr std::array<bool, 256> makeSeparators() {
  std::array<bool, 256> Separators{};
  for (char C : {' ', '\t', '\n', '\v', '\f', '\r'})
    Separators[static_cast<uint8_t>(C)] = true;
  return Separators;
}

constexpr auto Separators = makeSeparators();

/// The first separator in [\p Position, \p End), or \p End.
const char *findSeparator(const char *Position, const char *End) {
  // A separator is below 0x21 and nothing in a hex field is, so a field is
  // passed over eight bytes at a time until a word holds such a byte.
  constexpr uint64_t Ones = 0x0101010101010101ULL;
  while (End - Position >= 8) {
    uint64_t Word;
    std::memcpy(&Word, Position, sizeof(Word));
    if ((Word - Ones * 0x21) & ~Word & (Ones * 0x80))
      break;
    Position += 8;
  }
  while (Position != End && !Separators[static_cast<uint8_t>(*Position)])
    ++Position;
  return Position;
}

/// The whitespace-separated fields of \p Line, as llvm::SplitString gives
/// them.
void splitFields(llvm::StringRef Line,
                 llvm::SmallVectorImpl<llvm::StringRef> &Fields) {
  const char *Position = Line.begin();
  const char *const End = Line.end();
  while (true) {
    while (Position != End && Separators[static_cast<uint8_t>(*Position)])
      ++Position;
    if (Position == End)
      return;
    const char *const Start = Position;
    Position = findSeparator(Position, End);
    Fields.emplace_back(Start, static_cast<size_t>(Position - Start));
  }
}

bool isIgnorablePatternLine(llvm::StringRef Line) {
  Line = Line.trim();
  return Line.empty() || Line.starts_with(";") || Line.starts_with("#") ||
         Line == "---";
}

llvm::Error patternError(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}

/// A module being written into its chunk: offsets into the chunk's bytes
/// and names, which may still move while more lines are parsed into them.
struct DraftModule {
  size_t Bytes = 0;
  size_t Names = 0;
  uint32_t LeadingCount = 0;
  uint32_t TailCount = 0;
  uint32_t TotalLen = 0;
  uint32_t PublicNameCount = 0;
  uint32_t ReferenceCount = 0;
  uint16_t CRC16 = 0;
  uint8_t CRCLen = 0;
};

/// Where the modules of a chunk are written: each module's bytes, then its
/// stated bits, one after another in the room reserved for the chunk.
struct ChunkWriter {
  uint8_t *Bytes = nullptr;
  size_t Capacity = 0;
  size_t Used = 0;
  /// The stated bits of the module being parsed, bit I % 64 of word I / 64.
  std::vector<uint64_t> Bits;
  PatternNames &Names;
};

/// The room the modules of \p TextSize characters of lines can take; see
/// PatternParser::reserveBytes.
size_t byteCapacity(size_t TextSize) { return TextSize - TextSize / 4 + 1; }

llvm::Error noRoom() {
  return patternError("the module does not fit the room reserved for it");
}

/// Set \p Width bits of \p Mask in \p Bits from bit \p First on.
void orBits(uint64_t *Bits, size_t First, uint64_t Mask, unsigned Width) {
  Bits[First / 64] |= Mask << (First % 64);
  if (First % 64 + Width > 64)
    Bits[First / 64 + 1] |= Mask >> (64 - First % 64);
}

/// Decode \p Pairs character pairs of \p Text into \p Bytes, setting bit
/// \p FirstBit + I of \p Bits for each pair I that states a byte.  A pair of
/// hex digits states a byte and ".." leaves it unstated (zero); anything else
/// makes the whole call fail.
bool decodePairs(const char *Text, size_t Pairs, uint8_t *Bytes, uint64_t *Bits,
                 size_t FirstBit) {
  size_t I = 0;
#if defined(__SSE2__)
  // Eight pairs at a time: classify all sixteen characters, then fold each
  // pair's high and low characters within its 16-bit lane.
  const __m128i Zero = _mm_set1_epi8('0'), Nine = _mm_set1_epi8(9);
  const __m128i Case = _mm_set1_epi8(0x20), LowerA = _mm_set1_epi8('a');
  const __m128i Five = _mm_set1_epi8(5), Ten = _mm_set1_epi8(10);
  const __m128i Dot = _mm_set1_epi8('.'), LowByte = _mm_set1_epi16(0x00FF);
  __m128i Invalid = _mm_setzero_si128();
  for (; I + 8 <= Pairs; I += 8) {
    const __m128i Chars =
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(Text + 2 * I));
    const __m128i Digit = _mm_sub_epi8(Chars, Zero);
    const __m128i IsDigit = _mm_cmpeq_epi8(_mm_min_epu8(Digit, Nine), Digit);
    const __m128i Letter = _mm_sub_epi8(_mm_or_si128(Chars, Case), LowerA);
    const __m128i IsLetter = _mm_cmpeq_epi8(_mm_min_epu8(Letter, Five), Letter);
    const __m128i IsHex = _mm_or_si128(IsDigit, IsLetter);
    const __m128i Nibble =
        _mm_or_si128(_mm_and_si128(IsDigit, Digit),
                     _mm_andnot_si128(IsDigit, _mm_add_epi8(Letter, Ten)));
    const __m128i IsDot = _mm_cmpeq_epi8(Chars, Dot);
    const __m128i Stated =
        _mm_and_si128(_mm_and_si128(IsHex, _mm_srli_epi16(IsHex, 8)), LowByte);
    const __m128i Unstated =
        _mm_and_si128(_mm_and_si128(IsDot, _mm_srli_epi16(IsDot, 8)), LowByte);
    Invalid = _mm_or_si128(
        Invalid, _mm_andnot_si128(_mm_or_si128(Stated, Unstated), LowByte));
    const __m128i Value = _mm_and_si128(
        _mm_or_si128(_mm_slli_epi16(Nibble, 4), _mm_srli_epi16(Nibble, 8)),
        Stated);
    _mm_storel_epi64(reinterpret_cast<__m128i *>(Bytes + I),
                     _mm_packus_epi16(Value, Value));
    unsigned Mask = static_cast<unsigned>(_mm_movemask_epi8(Stated)) & 0x5555;
    Mask = (Mask | (Mask >> 1)) & 0x3333;
    Mask = (Mask | (Mask >> 2)) & 0x0F0F;
    Mask = (Mask | (Mask >> 4)) & 0x00FF;
    orBits(Bits, FirstBit + I, Mask, 8);
  }
  if (_mm_movemask_epi8(Invalid))
    return false;
#endif
  // The rest, and everything where there is no SSE2: the same rules a pair at
  // a time, without a branch on the data.
  bool Bad = false;
  while (I < Pairs) {
    const unsigned Width =
        static_cast<unsigned>(std::min<size_t>(8, Pairs - I));
    uint64_t Mask = 0;
    for (unsigned K = 0; K < Width; ++K) {
      const uint8_t High = static_cast<uint8_t>(Text[2 * (I + K)]);
      const uint8_t Low = static_cast<uint8_t>(Text[2 * (I + K) + 1]);
      const uint8_t HighDigit = High - '0', LowDigit = Low - '0';
      const uint8_t HighLetter = (High | 0x20) - 'a';
      const uint8_t LowLetter = (Low | 0x20) - 'a';
      const bool HighIsDigit = HighDigit < 10, LowIsDigit = LowDigit < 10;
      const bool Stated =
          (HighIsDigit || HighLetter < 6) && (LowIsDigit || LowLetter < 6);
      const uint8_t Value = static_cast<uint8_t>(
          ((HighIsDigit ? HighDigit : HighLetter + 10) << 4) |
          (LowIsDigit ? LowDigit : LowLetter + 10));
      Bad |= !Stated && !(High == '.' && Low == '.');
      Bytes[I + K] = Stated ? Value : 0;
      Mask |= uint64_t(Stated) << K;
    }
    orBits(Bits, FirstBit + I, Mask, Width);
    I += Width;
  }
  return !Bad;
}

/// Decode the hex pattern \p Pat as bytes \p Count onward of the module whose
/// bytes start at \p First in \p Writer's room.
llvm::Error appendPattern(llvm::StringRef Pat, ChunkWriter &Writer,
                          size_t First, uint32_t &Count) {
  if (Pat.size() % 2 != 0)
    return patternError("hex pattern has odd length");

  const size_t Length = Pat.size() / 2;
  if (Length > Writer.Capacity - First - Count)
    return noRoom();
  Writer.Bits.resize((Count + Length + 63) / 64);
  if (!decodePairs(Pat.data(), Length, Writer.Bytes + First + Count,
                   Writer.Bits.data(), Count)) {
    // Name the first pair that is neither two hex digits nor "..".
    for (size_t I = 0; I < Length; ++I) {
      const char High = Pat[2 * I], Low = Pat[2 * I + 1];
      const bool Stated = HexDigits[static_cast<uint8_t>(High)] <= 0xF &&
                          HexDigits[static_cast<uint8_t>(Low)] <= 0xF;
      if (!Stated && !(High == '.' && Low == '.'))
        return patternError("invalid hex byte: " + Pat.substr(2 * I, 2));
    }
  }
  Count += static_cast<uint32_t>(Length);
  return llvm::Error::success();
}

/// Parse the pattern line \p Line into \p Writer.  The names the module adds
/// refer to \p Line's text.  On failure the writer may hold part of the line,
/// and the caller discards it.
llvm::Expected<DraftModule> parsePatternLine(llvm::StringRef Line,
                                             ChunkWriter &Writer) {
  Line = Line.trim();
  DraftModule Mod;
  Mod.Bytes = Writer.Used;
  Mod.Names = Writer.Names.Names.size();
  Writer.Bits.clear();

  // Split into tokens by whitespace.
  llvm::SmallVector<llvm::StringRef, 16> Tokens;
  splitFields(Line, Tokens);

  if (Tokens.size() < 4)
    return patternError("too few fields in pattern line");

  // Token 0: hex pattern (leading bytes with .. wildcards).
  if (llvm::Error Error =
          appendPattern(Tokens[0], Writer, Mod.Bytes, Mod.LeadingCount))
    return std::move(Error);

  // Token 1: CRC length (hex byte).
  unsigned CRCLen;
  if (Tokens[1].getAsInteger(16, CRCLen) || CRCLen > 0xFFu)
    return patternError("invalid CRC length: " + Tokens[1]);
  Mod.CRCLen = static_cast<uint8_t>(CRCLen);

  // Token 2: CRC16 value (hex).
  unsigned CRC16Val;
  if (Tokens[2].getAsInteger(16, CRC16Val) || CRC16Val > 0xFFFFu)
    return patternError("invalid CRC16: " + Tokens[2]);
  Mod.CRC16 = static_cast<uint16_t>(CRC16Val);

  // Token 3: total length (hex).
  unsigned TotalLen;
  if (Tokens[3].getAsInteger(16, TotalLen))
    return patternError("invalid total length: " + Tokens[3]);
  Mod.TotalLen = TotalLen;
  if (Mod.TotalLen == 0)
    return patternError("total length must be non-zero");
  if (Mod.CRCLen != 0 && (Mod.LeadingCount > Mod.TotalLen ||
                          Mod.CRCLen > Mod.TotalLen - Mod.LeadingCount))
    return patternError("CRC range is outside total length");

  // Remaining tokens form :offset/name pairs, then ^offset/name references,
  // followed by at most one tail.
  bool SawTail = false;
  for (size_t I = 4; I < Tokens.size(); ++I) {
    if (Tokens[I].starts_with("^")) {
      if (SawTail)
        return patternError("reference follows the tail pattern");
      auto OffStr = Tokens[I].drop_front(1);
      unsigned Off;
      if (OffStr.empty() || OffStr.getAsInteger(16, Off))
        return patternError("invalid reference offset: " + Tokens[I]);
      if (Off >= Mod.TotalLen)
        return patternError("reference offset is outside total length");
      if (I + 1 >= Tokens.size() || Tokens[I + 1].starts_with(":") ||
          Tokens[I + 1].starts_with("^") || Tokens[I + 1].starts_with(".."))
        return patternError("reference name is missing after offset: " +
                            Tokens[I]);

      Writer.Names.Names.push_back({Off, Tokens[++I]});
      ++Mod.ReferenceCount;
      continue;
    }
    if (Tokens[I].starts_with(":")) {
      if (SawTail)
        return patternError("public name follows the tail pattern");
      if (Mod.ReferenceCount != 0)
        return patternError("public name follows a reference");
      auto OffStr = Tokens[I].drop_front(1);
      unsigned Off;
      if (OffStr.empty() || OffStr.getAsInteger(16, Off))
        return patternError("invalid public name offset: " + Tokens[I]);
      if (Off >= Mod.TotalLen)
        return patternError("public name offset is outside total length");
      if (I + 1 >= Tokens.size() || Tokens[I + 1].starts_with(":") ||
          Tokens[I + 1].starts_with("^") || Tokens[I + 1].starts_with(".."))
        return patternError("public name is missing after offset: " +
                            Tokens[I]);

      Writer.Names.Names.push_back({Off, Tokens[++I]});
      ++Mod.PublicNameCount;
      continue;
    }

    if (SawTail)
      return patternError("unexpected field after the tail pattern: " +
                          Tokens[I]);
    uint32_t Count = Mod.LeadingCount;
    if (llvm::Error Error = appendPattern(Tokens[I], Writer, Mod.Bytes, Count))
      return patternError("invalid tail pattern: " +
                          llvm::toString(std::move(Error)));
    Mod.TailCount = Count - Mod.LeadingCount;
    SawTail = true;
  }

  if (Mod.PublicNameCount == 0)
    return patternError("no public names found");

  // The stated bits follow the bytes.
  const size_t Count = size_t{Mod.LeadingCount} + Mod.TailCount;
  const size_t StatedBytes = (Count + 7) / 8;
  if (StatedBytes > Writer.Capacity - Mod.Bytes - Count)
    return noRoom();
  uint8_t *const Stated = Writer.Bytes + Mod.Bytes + Count;
  Writer.Bits.resize((Count + 63) / 64);
  for (size_t I = 0; I < StatedBytes; ++I)
    Stated[I] = static_cast<uint8_t>(Writer.Bits[I / 8] >> (I % 8 * 8));
  Writer.Used = Mod.Bytes + Count + StatedBytes;
  return Mod;
}

/// Copy \p Names into their own text, so they stop referring to the text
/// they were parsed from.
void keepNames(PatternNames &Names) {
  size_t Length = 0;
  for (const StoredName &Name : Names.Names)
    Length += Name.Name.size();
  Names.Text.resize(Length);
  char *Text = Names.Text.data();
  for (StoredName &Name : Names.Names) {
    std::memcpy(Text, Name.Name.data(), Name.Name.size());
    Name.Name = std::string_view(Text, Name.Name.size());
    Text += Name.Name.size();
  }
}

/// The module \p Draft describes, once its chunk no longer changes.
StoredModule storedModule(const DraftModule &Draft, const uint8_t *Bytes,
                          const PatternNames &Names) {
  StoredModule Module;
  Module.Bytes = Bytes + Draft.Bytes;
  Module.Stated = Module.Bytes + Draft.LeadingCount + Draft.TailCount;
  Module.Names = Names.Names.data() + Draft.Names;
  Module.LeadingCount = Draft.LeadingCount;
  Module.TailCount = Draft.TailCount;
  Module.TotalLen = Draft.TotalLen;
  Module.PublicNameCount = Draft.PublicNameCount;
  Module.ReferenceCount = Draft.ReferenceCount;
  Module.CRC16 = Draft.CRC16;
  Module.CRCLen = Draft.CRCLen;
  return Module;
}

} // namespace

PatternModule PatternParser::toPatternModule(const StoredModule &Module) {
  PatternModule Result;
  auto Pattern = [&](size_t First, size_t Count) {
    std::vector<PatternByte> Bytes(Count);
    for (size_t I = 0; I < Count; ++I) {
      Bytes[I].Value = Module.Bytes[First + I];
      Bytes[I].IsWildcard = !Module.isStated(First + I);
    }
    return Bytes;
  };
  Result.LeadingBytes = Pattern(0, Module.LeadingCount);
  Result.CRC16 = Module.CRC16;
  Result.CRCLen = Module.CRCLen;
  Result.TotalLen = Module.TotalLen;
  for (const StoredName &Name : Module.publicNames())
    Result.PublicNames.push_back({Name.Offset, std::string(Name.Name)});
  for (const StoredName &Name : Module.references())
    Result.References.push_back({Name.Offset, std::string(Name.Name)});
  Result.TailBytes = Pattern(Module.LeadingCount, Module.TailCount);
  return Result;
}

llvm::Expected<PatternModule> PatternParser::parseLine(llvm::StringRef Line) {
  if (isIgnorablePatternLine(Line))
    return patternError("skip line");
  std::vector<uint8_t> Bytes(byteCapacity(Line.size()));
  PatternNames Names;
  ChunkWriter Writer{Bytes.data(), Bytes.size(), 0, {}, Names};
  llvm::Expected<DraftModule> Draft = parsePatternLine(Line, Writer);
  if (!Draft)
    return Draft.takeError();
  return toPatternModule(storedModule(*Draft, Bytes.data(), Names));
}

std::vector<PatternChunk> PatternParser::splitChunks(llvm::StringRef Text,
                                                     size_t ChunkBytes) {
  ChunkBytes = std::max<size_t>(ChunkBytes, 1);
  std::vector<PatternChunk> Chunks;
  Chunks.reserve(Text.size() / ChunkBytes + 1);
  size_t Start = 0;
  while (Start < Text.size()) {
    // The chunk runs to the end of the line that holds its last byte.
    size_t End = Text.size();
    if (Text.size() - Start > ChunkBytes) {
      const size_t LineFeed = Text.find('\n', Start + ChunkBytes - 1);
      if (LineFeed != llvm::StringRef::npos)
        End = LineFeed + 1;
    }
    Chunks.emplace_back();
    Chunks.back().Text = Text.slice(Start, End);
    Start = End;
  }
  return Chunks;
}

std::unique_ptr<uint8_t[]>
PatternParser::reserveBytes(llvm::MutableArrayRef<PatternChunk> Chunks) {
  size_t Total = 0;
  for (const PatternChunk &Chunk : Chunks)
    Total += byteCapacity(Chunk.Text.size());
  // Left uninitialized: a page is only touched once a module is written there.
  std::unique_ptr<uint8_t[]> Bytes(new uint8_t[Total]);
  size_t Offset = 0;
  for (PatternChunk &Chunk : Chunks) {
    Chunk.Bytes = Bytes.get() + Offset;
    Chunk.ByteCapacity = byteCapacity(Chunk.Text.size());
    Offset += Chunk.ByteCapacity;
  }
  return Bytes;
}

void PatternParser::parseChunk(PatternChunk &Chunk) {
  ChunkWriter Writer{
      Chunk.Bytes, Chunk.Bytes ? Chunk.ByteCapacity : 0, 0, {}, Chunk.Names};
  std::vector<DraftModule> Drafts;
  llvm::StringRef Rest = Chunk.Text;
  size_t Line = 0;
  while (!Rest.empty()) {
    const size_t LineFeed = Rest.find('\n');
    const llvm::StringRef Text = Rest.take_front(LineFeed);
    Rest = LineFeed == llvm::StringRef::npos ? llvm::StringRef()
                                             : Rest.drop_front(LineFeed + 1);
    if (!isIgnorablePatternLine(Text)) {
      llvm::Expected<DraftModule> Draft = parsePatternLine(Text, Writer);
      if (!Draft) {
        Chunk.ErrorLine = Line;
        Chunk.Error = llvm::toString(Draft.takeError());
        return;
      }
      Drafts.push_back(*Draft);
    }
    ++Line;
  }
  Chunk.Lines = Line;

  keepNames(Chunk.Names);
  Chunk.Modules.reserve(Drafts.size());
  for (const DraftModule &Draft : Drafts)
    Chunk.Modules.push_back(storedModule(Draft, Chunk.Bytes, Chunk.Names));
}

void PatternParser::parseChunks(llvm::MutableArrayRef<PatternChunk> Chunks,
                                llvm::function_ref<void(size_t)> Parsed) {
  neverd::parallelForEach(Chunks.size(), [&](auto Claim, size_t Total) {
    for (size_t I = Claim(); I < Total; I = Claim()) {
      parseChunk(Chunks[I]);
      if (Parsed)
        Parsed(I);
    }
  });
}

llvm::Error PatternParser::firstError(llvm::ArrayRef<PatternChunk> Chunks) {
  // Every chunk before the first failure was parsed to its end, so their
  // line counts are whole.
  size_t LinesBefore = 0;
  for (const PatternChunk &Chunk : Chunks) {
    if (Chunk.ErrorLine)
      return patternError("pattern line " +
                          llvm::Twine(LinesBefore + *Chunk.ErrorLine + 1) +
                          ": " + Chunk.Error);
    LinesBefore += Chunk.Lines;
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<PatternModule>>
PatternParser::parseText(llvm::StringRef Text, size_t ChunkBytes) {
  std::vector<PatternChunk> Chunks = splitChunks(Text, ChunkBytes);
  const std::unique_ptr<uint8_t[]> Bytes = reserveBytes(Chunks);
  parseChunks(Chunks);
  if (llvm::Error Error = firstError(Chunks))
    return std::move(Error);

  std::vector<PatternModule> Modules;
  for (const PatternChunk &Chunk : Chunks)
    for (const StoredModule &Module : Chunk.Modules)
      Modules.push_back(toPatternModule(Module));
  return Modules;
}

llvm::Expected<std::vector<PatternModule>>
PatternParser::parseFile(const std::filesystem::path &Path) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path.string(), /*IsText=*/false,
                                              /*RequiresNullTerminator=*/false);
  if (!BufOrErr)
    return patternError("cannot open pattern file: " + Path.string());

  return parseText((*BufOrErr)->getBuffer());
}
