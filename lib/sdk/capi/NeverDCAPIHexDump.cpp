//===- NeverDCAPIHexDump.cpp - C API: hex dumps ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bytes of the image as hexadecimal text with a text column, in ASCII or
/// in any string encoding.
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/support/StringScan.h"

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>
#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

namespace {

constexpr int BytesPerLine = 16;

/// What the text column shows at one byte: a character's text at its first
/// byte, or a filler; Columns is the width it draws.
struct TextCell {
  std::string Text;
  unsigned Columns = 0;
};

/// The image's bytes in [Addr, Addr + Size), none where nothing is mapped or
/// materialized.
std::vector<std::optional<uint8_t>> readRange(const BinaryImage &Img, va_t Addr,
                                              size_t Size) {
  std::vector<std::optional<uint8_t>> Bytes(Size);
  for (size_t I = 0; I < Size;) {
    const va_t At = Addr + I;
    const Segment *Seg = Img.getSegmentFor(At);
    if (!Seg || At - Seg->VA >= Seg->Data.size()) {
      ++I;
      continue;
    }
    const size_t Offset = static_cast<size_t>(At - Seg->VA);
    const size_t Run = std::min(Size - I, Seg->Data.size() - Offset);
    for (size_t J = 0; J < Run; ++J)
      Bytes[I + J] = Seg->Data[Offset + J];
    I += Run;
  }
  return Bytes;
}

/// The text column's cells for \p Bytes read in \p Encoding.  A character's
/// first byte holds its text; its later bytes fill whatever of its byte
/// count the text does not draw, so each character keeps its bytes' width.
std::vector<TextCell>
textCells(const std::vector<std::optional<uint8_t>> &Bytes,
          strings::Encoding Encoding) {
  std::vector<TextCell> Cells(Bytes.size());
  for (size_t Start = 0; Start < Bytes.size();) {
    if (!Bytes[Start]) {
      Cells[Start] = {" ", 1};
      ++Start;
      continue;
    }
    size_t End = Start;
    std::vector<uint8_t> Run;
    while (End < Bytes.size() && Bytes[End])
      Run.push_back(*Bytes[End++]);
    strings::decode(
        Run, Encoding, [&](const strings::DecodedCharacter &Character) {
          const size_t At = Start + Character.Offset;
          unsigned Columns = 0;
          std::string Text;
          for (unsigned I = 0; I < Character.Codes; ++I) {
            if (!strings::isShownCharacter(Character.Code[I])) {
              Text.clear();
              break;
            }
            char Buffer[UNI_MAX_UTF8_BYTES_PER_CODE_POINT];
            char *Last = Buffer;
            llvm::ConvertCodePointToUTF8(Character.Code[I], Last);
            Text.append(Buffer, Last);
            Columns += strings::displayColumns(Character.Code[I]);
          }
          if (Text.empty()) {
            Text = ".";
            Columns = 1;
          }
          Cells[At] = {std::move(Text), Columns};
          for (unsigned I = 1; I < Character.Bytes; ++I)
            Cells[At + I] = I >= Columns ? TextCell{" ", 1} : TextCell{};
        });
    Start = End;
  }
  return Cells;
}

} // namespace

const char *neverd_hex_dump_ex(neverd_session_t Sess, neverd_va_t Addr,
                               int Size, const char *TextEncoding) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || Size <= 0)
    return nullptr;
  S->clearError();
  std::optional<strings::Encoding> Encoding;
  if (TextEncoding && *TextEncoding) {
    Encoding = strings::encodingNamed(TextEncoding);
    if (!Encoding) {
      S->setError(std::string("unknown text encoding: ") + TextEncoding);
      return nullptr;
    }
  }
  // The range stops at the end of the address space.
  const uint64_t Room = std::numeric_limits<va_t>::max() - Addr;
  const size_t Length = static_cast<uint64_t>(Size) - 1 <= Room
                            ? static_cast<size_t>(Size)
                            : static_cast<size_t>(Room + 1);
  const auto Bytes = readRange(S->Img, Addr, Length);
  size_t Mapped = Bytes.size();
  while (Mapped > 0 && !Bytes[Mapped - 1])
    --Mapped;
  if (!Mapped)
    return nullptr;
  std::vector<TextCell> Cells;
  if (Encoding)
    Cells = textCells(Bytes, *Encoding);
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  for (size_t Line = 0; Line < Mapped; Line += BytesPerLine) {
    OS << "0x" << llvm::utohexstr(Addr + Line) << "  ";
    const size_t Count = std::min<size_t>(BytesPerLine, Bytes.size() - Line);
    for (size_t J = 0; J < BytesPerLine; ++J) {
      if (J >= Count)
        OS << "   ";
      else if (const auto Byte = Bytes[Line + J])
        OS << llvm::format("%02x ", *Byte);
      else
        OS << "?? ";
      if (J == 7)
        OS << " ";
    }
    OS << " |";
    unsigned Columns = 0;
    for (size_t J = 0; J < Count; ++J) {
      if (Encoding) {
        OS << Cells[Line + J].Text;
        Columns += Cells[Line + J].Columns;
        continue;
      }
      const auto Byte = Bytes[Line + J];
      OS << (!Byte                           ? ' '
             : *Byte >= 0x20 && *Byte < 0x7f ? static_cast<char>(*Byte)
                                             : '.');
      ++Columns;
    }
    // A wide character that ends the line may draw past its last column.
    for (; Columns < Count; ++Columns)
      OS << ' ';
    OS << "|\n";
  }
  return dupStr(OS.str());
}

const char *neverd_hex_dump(neverd_session_t Sess, neverd_va_t Addr, int Size) {
  return neverd_hex_dump_ex(Sess, Addr, Size, nullptr);
}
