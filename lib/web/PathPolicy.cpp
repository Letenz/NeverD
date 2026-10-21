//===- PathPolicy.cpp - Portable member name policy --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Portable member name policy.
///
//===----------------------------------------------------------------------===//

#include "PathPolicy.h"

#include "Internal.h"

#include "neverd/web/Limits.h"

#ifdef NEVERD_WEB_ICU
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uversion.h>
#endif

namespace neverd::web {
bool archivePathsAvailable() {
#ifdef NEVERD_WEB_ICU
  UVersionInfo Version;
  u_getVersion(Version);
  UVersionInfo Unicode;
  u_getUnicodeVersion(Unicode);
  return Version[0] == 77 && Version[1] == 1 && Unicode[0] == 16 &&
         Unicode[1] == 0;
#else
  return false;
#endif
}

void validateMemberName(std::string_view Name) {
  if (Name.empty() || Name == "." || Name == ".." || Name.size() > 255 ||
      Name.back() == '.' || Name.back() == ' ' ||
      !llvm::json::isUTF8(llvm::StringRef(Name)))
    throw Error("unsafe_member_name");
  for (unsigned char C : Name)
    if (C < 32 || C == 127 ||
        std::string_view("/\\:*?\"<>|").find(C) != std::string_view::npos)
      throw Error("unsafe_member_name");
  auto Stem = Name.substr(0, Name.find('.'));
  std::string Upper(Stem);
  for (auto &C : Upper)
    if (C >= 'a' && C <= 'z')
      C -= 'a' - 'A';
  // Windows reserves superscript 1, 2 and 3 as COM/LPT device suffixes too.
  // This validation does not need or imply compatibility normalization.
  const auto Suffix =
      Upper.size() > 3 ? std::string_view(Upper).substr(3) : std::string_view{};
  if (Upper == "CON" || Upper == "PRN" || Upper == "AUX" || Upper == "NUL" ||
      ((Upper.starts_with("COM") || Upper.starts_with("LPT")) &&
       ((Suffix.size() == 1 && Suffix[0] >= '1' && Suffix[0] <= '9') ||
        Suffix == "¹" || Suffix == "²" || Suffix == "³")))
    throw Error("unsafe_member_name");
}

std::string nameKey(std::string_view Name) {
  validateMemberName(Name);
  bool ASCII = true;
  for (unsigned char C : Name)
    ASCII &= C < 128;
  if (ASCII) {
    // NFC and full folding on this admitted subset are exactly ASCII folding.
    // Safe ASCII capture stays available without the optional Unicode owner.
    std::string Key(Name);
    for (auto &C : Key)
      if (C >= 'A' && C <= 'Z')
        C += 'a' - 'A';
    return Key;
  }
  if (!archivePathsAvailable())
    throw Error("archive_path_policy_unavailable");
#ifdef NEVERD_WEB_ICU
  UErrorCode Status = U_ZERO_ERROR;
  const auto *NFC = icu::Normalizer2::getNFCInstance(Status);
  if (U_FAILURE(Status))
    throw Error("archive_path_policy_unavailable");
  auto Text = icu::UnicodeString::fromUTF8(
      icu::StringPiece(Name.data(), int32_t(Name.size())));
  icu::UnicodeString Normalized;
  NFC->normalize(Text, Normalized, Status);
  Normalized.foldCase(U_FOLD_CASE_DEFAULT);
  Text.remove();
  NFC->normalize(Normalized, Text, Status);
  if (U_FAILURE(Status))
    throw Error("archive_path_policy_unavailable");
  std::string Key;
  Text.toUTF8String(Key);
  return Key;
#else
  throw Error("archive_path_policy_unavailable");
#endif
}

std::string archivePathKey(std::string_view Path) {
  if (!archivePathsAvailable())
    throw Error("archive_path_policy_unavailable");
  if (Path.empty() ||
      Path.size() >
          ::neverd::limits::web::kPathPolicyArchivePathKeyPathSizeLimit)
    throw Error("unsafe_member_name");
  std::string Key;
  while (!Path.empty()) {
    const auto Slash = Path.find('/');
    const auto Name = Path.substr(0, Slash);
    if (!Key.empty())
      Key += '/';
    Key += nameKey(Name);
    if (Slash == std::string_view::npos)
      break;
    Path.remove_prefix(Slash + 1);
    if (Path.empty())
      throw Error("unsafe_member_name");
  }
  return Key;
}
} // namespace neverd::web
