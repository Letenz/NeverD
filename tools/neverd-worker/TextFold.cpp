#include "TextFold.h"

#include "EngineSymbols.h"

#include "neverd/sdk/NeverDCAPI.h"

#include <algorithm>

namespace neverd::worker {
namespace {

using FoldFunction = const char *(*)(const char *);

char lowerASCII(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::string foldText(std::string_view text) {
  std::string folded(text);
  const bool ascii = std::all_of(text.begin(), text.end(), [](char c) {
    return static_cast<unsigned char>(c) < 0x80;
  });
  static const auto fold = engineSymbol<FoldFunction>("neverd_fold_case");
  if (ascii || !fold) {
    std::transform(folded.begin(), folded.end(), folded.begin(), lowerASCII);
    return folded;
  }
  const char *raw = fold(folded.c_str());
  if (raw) {
    folded = raw;
    neverd_free_string(raw);
  }
  return folded;
}

} // namespace neverd::worker
