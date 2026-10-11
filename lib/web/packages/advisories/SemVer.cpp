//===- SemVer.cpp - Strict metadata version admission --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SemVer 2.0.0 grammar and precedence with bounded, arbitrary-width numbers.
///
//===----------------------------------------------------------------------===//

#include "SemVer.h"

namespace neverd::web::package_advisories {
namespace {
bool digits(std::string_view Text) {
  return !Text.empty() && Text.find_first_not_of("0123456789") == Text.npos;
}
bool number(std::string_view Text) {
  return digits(Text) && (Text.size() == 1 || Text.front() != '0');
}
std::string_view take(std::string_view &Text, char Delimiter) {
  const auto End = Text.find(Delimiter);
  const auto Part = Text.substr(0, End);
  Text = End == Text.npos ? std::string_view{} : Text.substr(End + 1);
  return Part;
}
bool identifiers(std::string_view Text, std::vector<std::string> *Parts) {
  if (Text.empty() || Text.back() == '.')
    return false;
  while (!Text.empty()) {
    const auto Part = take(Text, '.');
    if (Part.empty() ||
        Part.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijk"
                               "lmnopqrstuvwxyz-") != Part.npos ||
        (Parts && digits(Part) && !number(Part)))
      return false;
    if (Parts)
      Parts->emplace_back(Part);
  }
  return true;
}
int lexical(std::string_view Before, std::string_view After) {
  return Before == After ? 0 : Before < After ? -1 : 1;
}
int numeric(std::string_view Before, std::string_view After) {
  if (Before.size() != After.size())
    return Before.size() < After.size() ? -1 : 1;
  return lexical(Before, After);
}
} // namespace

std::optional<SemVer> parseSemVer(std::string_view Text) {
  if (Text.empty() || Text.size() > MaxSemVerBytes)
    return {};
  SemVer Result;
  const auto Plus = Text.find('+');
  if (Plus != Text.npos) {
    if (!identifiers(Text.substr(Plus + 1), nullptr))
      return {};
    Text = Text.substr(0, Plus);
  }
  const auto Dash = Text.find('-');
  if (Dash != Text.npos) {
    if (!identifiers(Text.substr(Dash + 1), &Result.Prerelease))
      return {};
    Text = Text.substr(0, Dash);
  }
  for (unsigned I = 0; I != 3; ++I) {
    const auto Dot = Text.find('.');
    if ((I < 2) != (Dot != Text.npos))
      return {};
    const auto Part = take(Text, '.');
    if (!number(Part))
      return {};
    Result.Core[I] = Part;
  }
  return Result;
}

int compareSemVer(const SemVer &Before, const SemVer &After) {
  for (unsigned I = 0; I != 3; ++I)
    if (const auto Order = numeric(Before.Core[I], After.Core[I]))
      return Order;
  if (Before.Prerelease.empty() != After.Prerelease.empty())
    return Before.Prerelease.empty() ? 1 : -1;
  for (size_t I = 0;
       I < Before.Prerelease.size() && I < After.Prerelease.size(); ++I) {
    const auto &B = Before.Prerelease[I], &A = After.Prerelease[I];
    const bool BN = digits(B), AN = digits(A);
    if (BN != AN)
      return BN ? -1 : 1;
    if (const auto Order = BN ? numeric(B, A) : lexical(B, A))
      return Order;
  }
  return Before.Prerelease.size() == After.Prerelease.size()  ? 0
         : Before.Prerelease.size() < After.Prerelease.size() ? -1
                                                              : 1;
}
} // namespace neverd::web::package_advisories
