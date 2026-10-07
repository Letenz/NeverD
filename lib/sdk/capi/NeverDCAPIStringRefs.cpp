//===- NeverDCAPIStringRefs.cpp - C API: string references ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The instructions that refer to string text: each function's direct
/// references (InstructionFlow.h) meet the image's strings (ImageStrings.h).
///
//===----------------------------------------------------------------------===//

#include "ImageStrings.h"
#include "InstructionFlow.h"
#include "JSONText.h"
#include "SessionImpl.h"

#include "llvm/Support/JSON.h"

using namespace neverd;
using namespace neverd::sdk;

namespace {

/// The text a reference to \p Address reads: the string holding it and
/// where the text from its first whole character on begins in the string's
/// UTF-8 text, while at least \p MinChars characters remain there.
struct ReferredText {
  const ImageString *String = nullptr;
  uint64_t TextOffset = 0;
};

std::optional<ReferredText>
referredText(const BinaryImage &Img, const std::vector<ImageString> &Strings,
             unsigned MinChars, va_t Address) {
  const ImageString *String = stringHolding(Strings, Address);
  if (!String)
    return std::nullopt;
  // The whole string was found with its own minimum length.
  if (Address == String->Address)
    return ReferredText{String, 0};
  const uint8_t *Bytes = Img.readVA(String->Address, String->Bytes);
  if (!Bytes)
    return std::nullopt;
  const auto Start =
      strings::textFrom(llvm::ArrayRef<uint8_t>(Bytes, String->Bytes),
                        String->Kind, Address - String->Address);
  if (!Start || Start->Chars < MinChars)
    return std::nullopt;
  return ReferredText{String, Start->UTF8};
}

} // namespace

const char *neverd_string_refs_json(neverd_session_t Sess,
                                    const char *OptionsJson,
                                    neverd_va_t FirstEntry, int MaxFunctions) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
    S->setError("string references are only published for native images");
    return nullptr;
  }
  strings::ScanOptions Options;
  if (const auto Error = parseStringOptions(OptionsJson, Options);
      !Error.empty()) {
    S->setError(Error);
    return nullptr;
  }
  if (!S->synchronizeFunctions())
    return nullptr;
  const std::vector<ImageString> &Strings = imageStrings(*S, Options);
  const unsigned MinChars = std::max(1u, Options.MinChars);
  const FunctionPage Page = functionPage(*S, FirstEntry, MaxFunctions);
  struct Row {
    va_t From, To;
    ReferredText Text;
    llvm::StringRef Kind;
    std::optional<va_t> Via;
  };
  // Each function's rows, merged in entry order.
  std::vector<std::vector<Row>> Slots(Page.Functions.size());
  const BinaryImage &Img = S->Img;
  const bool Decoded = decodeFunctions(
      *S, Page, [&](size_t Index, Decoder &Dec, const DecodedInsn &DI) {
        const InstructionFlow Flow = summarizeInstructionFlow(Dec, DI);
        for (const auto &[To, Kind] : Flow.Refs) {
          if (const auto Text = referredText(Img, Strings, MinChars, To)) {
            Slots[Index].push_back({DI.Addr, To, *Text, Kind, std::nullopt});
            continue;
          }
          // A slot the loader relocated holds a pointer for certain.
          if (!Img.DataPtrRelocSlots.count(To) &&
              !Img.CodePtrRelocSlots.count(To))
            continue;
          if (const auto Target = relocatedPointer(Img, To))
            if (const auto Text = referredText(Img, Strings, MinChars, *Target))
              Slots[Index].push_back({DI.Addr, *Target, *Text, Kind, To});
        }
      });
  if (!Decoded) {
    S->setError("failed to initialize a decoder for the image");
    return nullptr;
  }
  llvm::json::Array Refs;
  for (const auto &Slot : Slots)
    for (const Row &R : Slot)
      Refs.push_back(llvm::json::Array{
          vaHex(R.From), vaHex(R.To), vaHex(R.Text.String->Address),
          static_cast<int64_t>(R.Text.TextOffset), R.Kind.str(),
          R.Via ? llvm::json::Value(vaHex(*R.Via)) : nullptr});
  llvm::json::Object Result;
  Result["refs"] = std::move(Refs);
  Result["next_entry"] =
      Page.NextEntry ? llvm::json::Value(vaHex(*Page.NextEntry)) : nullptr;
  Result["function_count"] = static_cast<int64_t>(S->Functions.size());
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}
