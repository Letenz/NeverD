//===- ImageStrings.cpp - The strings of a loaded image -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ImageStrings.h"

#include "SessionImpl.h"

#include "llvm/Support/JSON.h"

#include <algorithm>

namespace neverd::sdk {

std::string parseStringOptions(const char *Json,
                               strings::ScanOptions &Options) {
  if (!Json || !*Json)
    return {};
  auto Parsed = llvm::json::parse(Json);
  const llvm::json::Object *Object = Parsed ? Parsed->getAsObject() : nullptr;
  if (!Object) {
    if (!Parsed)
      llvm::consumeError(Parsed.takeError());
    return "string options must be a JSON object";
  }
  for (const auto &[Key, Value] : *Object) {
    if (Key == "min_length") {
      const auto Length = Value.getAsInteger();
      if (!Length || *Length < 1 || *Length > strings::MaxMinLength)
        return "min_length must be an integer from 1 to " +
               std::to_string(strings::MaxMinLength);
      Options.MinLength = static_cast<unsigned>(*Length);
    } else if (Key == "encodings") {
      const auto *Names = Value.getAsArray();
      if (!Names)
        return "encodings must be an array of encoding names";
      Options.Encodings = 0;
      for (const auto &Name : *Names) {
        const auto Text = Name.getAsString();
        const auto Encoding =
            Text ? strings::encodingNamed(*Text) : std::nullopt;
        if (!Encoding)
          return "unknown string encoding: " +
                 (Text ? Text->str() : std::string("non-string"));
        Options.Encodings |= strings::encodingBit(*Encoding);
      }
    } else if (Key == "preferred") {
      if (Value.kind() == llvm::json::Value::Null) {
        Options.Preferred.reset();
        continue;
      }
      const auto Text = Value.getAsString();
      const auto Encoding = Text ? strings::encodingNamed(*Text) : std::nullopt;
      if (!Encoding || !strings::isLegacyEncoding(*Encoding))
        return "preferred must name a legacy code page";
      Options.Preferred = *Encoding;
    } else {
      return "unknown string option: " + Key.str();
    }
  }
  return {};
}

const std::vector<ImageString> &
imageStrings(Session &S, const strings::ScanOptions &Options) {
  if (S.ImageStrings &&
      S.ImageStrings->Options.Encodings == Options.Encodings &&
      S.ImageStrings->Options.Preferred == Options.Preferred &&
      S.ImageStrings->Options.MinLength == Options.MinLength)
    return S.ImageStrings->Strings;
  ImageStringScan Scan;
  Scan.Options = Options;
  const auto ScanRange = [&](va_t Start, const uint8_t *Bytes, size_t Size) {
    strings::scan(llvm::ArrayRef<uint8_t>(Bytes, Size), Options,
                  [&](strings::FoundString &&Found) {
                    Scan.Strings.push_back({Start + Found.Offset, Found.Bytes,
                                            Found.Chars, Found.Kind,
                                            std::move(Found.Text)});
                  });
  };
  // Data segments whole, and the data sections of code segments.
  for (const auto &Seg : S.Img.Segments) {
    if (!Seg.isExecutable()) {
      ScanRange(Seg.VA, Seg.Data.data(), Seg.Data.size());
      continue;
    }
    for (const auto &Sec : S.Img.Sections) {
      if (Sec.isExecutable() || !Sec.FileSz || !Seg.contains(Sec.VA) ||
          Sec.VA - Seg.VA >= Seg.Data.size())
        continue;
      const uint64_t Offset = Sec.VA - Seg.VA;
      const uint64_t Size =
          std::min<uint64_t>({Sec.Size, Sec.FileSz, Seg.Data.size() - Offset});
      ScanRange(Sec.VA, Seg.Data.data() + Offset, static_cast<size_t>(Size));
    }
  }
  // Ranges are scanned in image order, which need not be address order.
  std::stable_sort(Scan.Strings.begin(), Scan.Strings.end(),
                   [](const ImageString &A, const ImageString &B) {
                     return A.Address < B.Address;
                   });
  S.ImageStrings = std::move(Scan);
  return S.ImageStrings->Strings;
}

const ImageString *stringHolding(const std::vector<ImageString> &Strings,
                                 va_t Address) {
  auto It = std::upper_bound(Strings.begin(), Strings.end(), Address,
                             [](va_t Value, const ImageString &String) {
                               return Value < String.Address;
                             });
  if (It == Strings.begin())
    return nullptr;
  --It;
  return Address - It->Address < It->Bytes ? &*It : nullptr;
}

} // namespace neverd::sdk
