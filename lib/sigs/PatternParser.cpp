//===- PatternParser.cpp - FLIRT .pat text format parser -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/PatternParser.h"

#include "neverd/support/Parallel.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MemoryBuffer.h"

#include <array>
#include <string>

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
    while (Position != End && !Separators[static_cast<uint8_t>(*Position)])
      ++Position;
    Fields.emplace_back(Start, static_cast<size_t>(Position - Start));
  }
}

bool isIgnorablePatternLine(llvm::StringRef Line) {
  Line = Line.trim();
  return Line.empty() || Line.starts_with(";") || Line.starts_with("#") ||
         Line == "---";
}

} // namespace

bool PatternParser::parseHexByte(llvm::StringRef Hex, uint8_t &Out) {
  if (Hex.size() != 2)
    return false;
  const uint8_t High = HexDigits[static_cast<uint8_t>(Hex[0])];
  const uint8_t Low = HexDigits[static_cast<uint8_t>(Hex[1])];
  if ((High | Low) > 0xF)
    return false;
  Out = static_cast<uint8_t>((High << 4) | Low);
  return true;
}

llvm::Expected<std::vector<PatternByte>>
PatternParser::parseHexPattern(llvm::StringRef Pat) {
  if (Pat.size() % 2 != 0)
    return llvm::make_error<llvm::StringError>("hex pattern has odd length",
                                               llvm::inconvertibleErrorCode());

  std::vector<PatternByte> Result(Pat.size() / 2);
  for (size_t I = 0; I < Result.size(); ++I) {
    const char HighChar = Pat[2 * I], LowChar = Pat[2 * I + 1];
    const uint8_t High = HexDigits[static_cast<uint8_t>(HighChar)];
    const uint8_t Low = HexDigits[static_cast<uint8_t>(LowChar)];
    if ((High | Low) <= 0xF) {
      Result[I].Value = static_cast<uint8_t>((High << 4) | Low);
      continue;
    }
    if (HighChar == '.' && LowChar == '.') {
      Result[I].IsWildcard = true;
      continue;
    }
    return llvm::make_error<llvm::StringError>("invalid hex byte: " +
                                                   Pat.substr(2 * I, 2).str(),
                                               llvm::inconvertibleErrorCode());
  }
  return Result;
}

llvm::Expected<PatternModule> PatternParser::parseLine(llvm::StringRef Line) {
  Line = Line.trim();
  if (Line.empty() || Line.starts_with(";") || Line.starts_with("#") ||
      Line == "---")
    return llvm::make_error<llvm::StringError>("skip line",
                                               llvm::inconvertibleErrorCode());

  PatternModule Mod;

  // Split into tokens by whitespace.
  llvm::SmallVector<llvm::StringRef, 16> Tokens;
  splitFields(Line, Tokens);

  if (Tokens.size() < 4)
    return llvm::make_error<llvm::StringError>("too few fields in pattern line",
                                               llvm::inconvertibleErrorCode());

  // Token 0: hex pattern (leading bytes with .. wildcards).
  auto LeadingOrErr = parseHexPattern(Tokens[0]);
  if (!LeadingOrErr)
    return LeadingOrErr.takeError();
  Mod.LeadingBytes = std::move(*LeadingOrErr);

  // Token 1: CRC length (hex byte).
  unsigned CRCLen;
  if (Tokens[1].getAsInteger(16, CRCLen) || CRCLen > 0xFFu)
    return llvm::make_error<llvm::StringError>("invalid CRC length: " +
                                                   Tokens[1].str(),
                                               llvm::inconvertibleErrorCode());
  Mod.CRCLen = static_cast<uint8_t>(CRCLen);

  // Token 2: CRC16 value (hex).
  unsigned CRC16Val;
  if (Tokens[2].getAsInteger(16, CRC16Val) || CRC16Val > 0xFFFFu)
    return llvm::make_error<llvm::StringError>(
        "invalid CRC16: " + Tokens[2].str(), llvm::inconvertibleErrorCode());
  Mod.CRC16 = static_cast<uint16_t>(CRC16Val);

  // Token 3: total length (hex).
  unsigned TotalLen;
  if (Tokens[3].getAsInteger(16, TotalLen))
    return llvm::make_error<llvm::StringError>("invalid total length: " +
                                                   Tokens[3].str(),
                                               llvm::inconvertibleErrorCode());
  Mod.TotalLen = TotalLen;
  if (Mod.TotalLen == 0)
    return llvm::make_error<llvm::StringError>("total length must be non-zero",
                                               llvm::inconvertibleErrorCode());
  if (Mod.CRCLen != 0 && (Mod.LeadingBytes.size() > Mod.TotalLen ||
                          Mod.CRCLen > Mod.TotalLen - Mod.LeadingBytes.size()))
    return llvm::make_error<llvm::StringError>(
        "CRC range is outside total length", llvm::inconvertibleErrorCode());

  // Remaining tokens form :offset/name pairs, then ^offset/name references,
  // followed by at most one tail.
  bool SawTail = false;
  for (size_t I = 4; I < Tokens.size(); ++I) {
    if (Tokens[I].starts_with("^")) {
      if (SawTail)
        return llvm::make_error<llvm::StringError>(
            "reference follows the tail pattern",
            llvm::inconvertibleErrorCode());
      auto OffStr = Tokens[I].drop_front(1);
      unsigned Off;
      if (OffStr.empty() || OffStr.getAsInteger(16, Off))
        return llvm::make_error<llvm::StringError>(
            "invalid reference offset: " + Tokens[I].str(),
            llvm::inconvertibleErrorCode());
      if (Off >= Mod.TotalLen)
        return llvm::make_error<llvm::StringError>(
            "reference offset is outside total length",
            llvm::inconvertibleErrorCode());
      if (I + 1 >= Tokens.size() || Tokens[I + 1].starts_with(":") ||
          Tokens[I + 1].starts_with("^") || Tokens[I + 1].starts_with(".."))
        return llvm::make_error<llvm::StringError>(
            "reference name is missing after offset: " + Tokens[I].str(),
            llvm::inconvertibleErrorCode());

      FuncRef Ref;
      Ref.Offset = Off;
      Ref.Name = Tokens[++I].str();
      Mod.References.push_back(std::move(Ref));
      continue;
    }
    if (Tokens[I].starts_with(":")) {
      if (SawTail)
        return llvm::make_error<llvm::StringError>(
            "public name follows the tail pattern",
            llvm::inconvertibleErrorCode());
      if (!Mod.References.empty())
        return llvm::make_error<llvm::StringError>(
            "public name follows a reference", llvm::inconvertibleErrorCode());
      auto OffStr = Tokens[I].drop_front(1);
      unsigned Off;
      if (OffStr.empty() || OffStr.getAsInteger(16, Off))
        return llvm::make_error<llvm::StringError>(
            "invalid public name offset: " + Tokens[I].str(),
            llvm::inconvertibleErrorCode());
      if (Off >= Mod.TotalLen)
        return llvm::make_error<llvm::StringError>(
            "public name offset is outside total length",
            llvm::inconvertibleErrorCode());
      if (I + 1 >= Tokens.size() || Tokens[I + 1].starts_with(":") ||
          Tokens[I + 1].starts_with("^") || Tokens[I + 1].starts_with(".."))
        return llvm::make_error<llvm::StringError>(
            "public name is missing after offset: " + Tokens[I].str(),
            llvm::inconvertibleErrorCode());

      FuncRef Ref;
      Ref.Offset = Off;
      Ref.Name = Tokens[++I].str();
      Mod.PublicNames.push_back(std::move(Ref));
      continue;
    }

    if (SawTail)
      return llvm::make_error<llvm::StringError>(
          "unexpected field after the tail pattern: " + Tokens[I].str(),
          llvm::inconvertibleErrorCode());
    auto TailOrErr = parseHexPattern(Tokens[I]);
    if (!TailOrErr)
      return llvm::make_error<llvm::StringError>(
          "invalid tail pattern: " + llvm::toString(TailOrErr.takeError()),
          llvm::inconvertibleErrorCode());
    Mod.TailBytes = std::move(*TailOrErr);
    SawTail = true;
  }

  if (Mod.PublicNames.empty())
    return llvm::make_error<llvm::StringError>("no public names found",
                                               llvm::inconvertibleErrorCode());

  return Mod;
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

void PatternParser::parseChunk(PatternChunk &Chunk) {
  llvm::StringRef Rest = Chunk.Text;
  size_t Line = 0;
  while (!Rest.empty()) {
    const size_t LineFeed = Rest.find('\n');
    const llvm::StringRef Text = Rest.take_front(LineFeed);
    Rest = LineFeed == llvm::StringRef::npos ? llvm::StringRef()
                                             : Rest.drop_front(LineFeed + 1);
    if (!isIgnorablePatternLine(Text)) {
      auto ModOrErr = parseLine(Text);
      if (!ModOrErr) {
        Chunk.ErrorLine = Line;
        Chunk.Error = llvm::toString(ModOrErr.takeError());
        return;
      }
      Chunk.Modules.push_back(std::move(*ModOrErr));
    }
    ++Line;
  }
  Chunk.Lines = Line;
}

void PatternParser::parseChunks(llvm::MutableArrayRef<PatternChunk> Chunks) {
  neverd::parallelForEach(Chunks.size(), [&](auto Claim, size_t Total) {
    for (size_t I = Claim(); I < Total; I = Claim())
      parseChunk(Chunks[I]);
  });
}

llvm::Error PatternParser::firstError(llvm::ArrayRef<PatternChunk> Chunks) {
  // Every chunk before the first failure was parsed to its end, so their
  // line counts are whole.
  size_t LinesBefore = 0;
  for (const PatternChunk &Chunk : Chunks) {
    if (Chunk.ErrorLine)
      return llvm::make_error<llvm::StringError>(
          "pattern line " + std::to_string(LinesBefore + *Chunk.ErrorLine + 1) +
              ": " + Chunk.Error,
          llvm::inconvertibleErrorCode());
    LinesBefore += Chunk.Lines;
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<PatternModule>>
PatternParser::parseText(llvm::StringRef Text, size_t ChunkBytes) {
  std::vector<PatternChunk> Chunks = splitChunks(Text, ChunkBytes);
  parseChunks(Chunks);
  if (llvm::Error Error = firstError(Chunks))
    return std::move(Error);

  size_t Count = 0;
  for (const PatternChunk &Chunk : Chunks)
    Count += Chunk.Modules.size();
  std::vector<PatternModule> Modules;
  Modules.reserve(Count);
  for (PatternChunk &Chunk : Chunks)
    Modules.insert(Modules.end(),
                   std::make_move_iterator(Chunk.Modules.begin()),
                   std::make_move_iterator(Chunk.Modules.end()));
  return Modules;
}

llvm::Expected<std::vector<PatternModule>>
PatternParser::parseFile(const std::filesystem::path &Path) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path.string(), /*IsText=*/false,
                                              /*RequiresNullTerminator=*/false);
  if (!BufOrErr)
    return llvm::make_error<llvm::StringError>("cannot open pattern file: " +
                                                   Path.string(),
                                               llvm::inconvertibleErrorCode());

  return parseText((*BufOrErr)->getBuffer());
}
