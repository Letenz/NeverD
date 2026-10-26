//===- PatchJSON.cpp - Byte patch rows ------------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded, lossless byte patch persistence shared by every adapter.
//===----------------------------------------------------------------------===//

#include "neverd/patch/Limits.h"
#include "neverd/patch/PatchIO.h"

#include "llvm/ADT/StringExtras.h"

#include <string>

using namespace neverd::patch;
namespace patchLimits = neverd::limits::patch;

static llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}

llvm::Expected<std::vector<uint8_t>>
neverd::patch::parseHex(llvm::StringRef Text, uint64_t Limit) {
  if (Text.size() % 2 || Text.size() / 2 > Limit)
    return invalid("hex bytes need pairs of digits within the byte limit");
  std::vector<uint8_t> Bytes;
  Bytes.reserve(Text.size() / 2);
  for (size_t I = 0; I < Text.size(); I += 2) {
    const unsigned H = llvm::hexDigitValue(Text[I]);
    const unsigned L = llvm::hexDigitValue(Text[I + 1]);
    if (H > 15 || L > 15)
      return invalid("invalid hexadecimal byte");
    Bytes.push_back((H << 4) | L);
  }
  return Bytes;
}

void neverd::patch::writePatchRows(llvm::json::OStream &Output,
                                   const PatchOverlay &Overlay) {
  Output.array([&] {
    for (const auto &[Offset, Row] : Overlay.rows())
      Output.object([&] {
        const std::string FileOffset = "0x" + llvm::utohexstr(Offset, true);
        Output.attribute("addr", llvm::StringRef(FileOffset));
        Output.attribute("current", llvm::toHex(Row.Current, true));
        Output.attribute("file_off", llvm::StringRef(FileOffset));
        Output.attribute("kind", patchKindName(Row.Kind));
        Output.attribute("original", llvm::toHex(Row.Original, true));
        Output.attribute("size", Row.size());
        Output.attribute("va", "0x" + llvm::utohexstr(Row.Address, true));
      });
  });
}

llvm::Expected<std::vector<PatchRow>>
neverd::patch::parsePatchRows(llvm::StringRef Text) {
  if (Text.size() > patchLimits::kMaxPatchesFileBytes)
    return invalid("patches.json exceeds its size limit");
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed)
    return Parsed.takeError();
  const auto *Array = Parsed->getAsArray();
  if (!Array || Array->size() > patchLimits::kMaxPatchRows)
    return invalid("patches.json must be a bounded array");
  std::vector<PatchRow> Rows;
  uint64_t Total = 0;
  for (const auto &Value : *Array) {
    const auto *Object = Value.getAsObject();
    if (!Object)
      return invalid("patch row must be an object");
    PatchRow Row;
    const auto ReadAddress = [&](llvm::StringRef Key, uint64_t &Out) {
      auto S = Object->getString(Key);
      return S && S->consume_front("0x") && !S->empty() &&
             !S->getAsInteger(16, Out);
    };
    const auto *SizeValue = Object->get("size");
    auto Size =
        SizeValue ? SizeValue->getAsUINT64() : std::optional<uint64_t>();
    auto Original = Object->getString("original");
    auto Current = Object->getString("current");
    auto Kind = Object->getString("kind");
    if (!ReadAddress("file_off", Row.FileOffset) ||
        !ReadAddress("va", Row.Address) || !Size || !*Size ||
        *Size > patchLimits::kMaxPatchedBytes - Total || !Original ||
        !Current || !Kind || !parsePatchKind(*Kind))
      return invalid("invalid patch row fields");
    if (Object->get("addr")) {
      uint64_t Key = 0;
      if (!ReadAddress("addr", Key) || Key != Row.FileOffset)
        return invalid("patch row key differs from file offset");
    }
    auto O = parseHex(*Original, patchLimits::kMaxPatchedBytes);
    auto C = parseHex(*Current, patchLimits::kMaxPatchedBytes);
    if (!O)
      return O.takeError();
    if (!C)
      return C.takeError();
    if (O->size() != *Size || C->size() != *Size)
      return invalid("patch row size does not match its bytes");
    Total += *Size;
    Row.Original = std::move(*O);
    Row.Current = std::move(*C);
    Row.Kind = *parsePatchKind(*Kind);
    Rows.push_back(std::move(Row));
  }
  return Rows;
}
