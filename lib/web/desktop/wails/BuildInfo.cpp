//===- BuildInfo.cpp - Declared Go and Wails layout versions --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded Go 1.25.7 inline build-info framing. Declarations are untrusted
/// evidence and never establish publisher or runtime identity.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web::wails_detail {
namespace {
std::string inlineString(Reader &R, uint64_t &At, Range Owner) {
  uint64_t Size = 0;
  unsigned Shift = 0, Count = 0;
  for (;;) {
    R.step();
    if (!contains(Owner, {At, 1}))
      throw Error("wails_invalid_build_info");
    const auto Byte = uint8_t(R.read({At++, 1})[0]);
    ++Count;
    if (Shift == 63 && Byte > 1)
      throw Error("wails_invalid_build_info");
    Size |= uint64_t(Byte & 127) << Shift;
    if (!(Byte & 128)) {
      if (Count > 1 && Byte == 0)
        throw Error("wails_invalid_build_info");
      break;
    }
    if (Count == 10)
      throw Error("wails_invalid_build_info");
    Shift += 7;
  }
  if (Size > ::neverd::limits::web::kBuildInfoInlineStringSizeLimit ||
      !contains(Owner, {At, Size}))
    throw Error("wails_build_info_budget_exceeded");
  const auto S = R.read({At, Size});
  At += Size;
  return S;
}
void declarations(std::string_view Mod, std::string_view Platform) {
  constexpr std::string_view Start =
      "\x30\x77\xaf\x0c\x92\x74\x08\x02\x41\xe1\xc1\x07\xe6\xd6\x18\xe6";
  constexpr std::string_view End{
      "\xf9\x32\x43\x31\x86\x18\x20\x72\x00\x82\x42\x10\x41\x16\xd8\xf2", 16};
  if (Mod.size() < 33 || !Mod.starts_with(Start) || !Mod.ends_with(End))
    throw Error("wails_invalid_build_info");
  Mod.remove_prefix(16);
  Mod.remove_suffix(16);
  if (Mod.back() != '\n')
    throw Error("wails_invalid_build_info");
  unsigned Wails = 0, Arch = 0, OS = 0;
  bool PreviousWails = false;
  while (!Mod.empty()) {
    const auto At = Mod.find('\n');
    if (At == std::string_view::npos)
      throw Error("wails_invalid_build_info");
    const auto Line = Mod.substr(0, At);
    Mod.remove_prefix(At + 1);
    if (PreviousWails && Line.starts_with("=>\t"))
      throw Error("wails_replaced_framework_declaration");
    PreviousWails = false;
    constexpr std::string_view Prefix = "dep\tgithub.com/wailsapp/wails/v2\t";
    if (Line.starts_with(Prefix)) {
      const auto Value = Line.substr(Prefix.size());
      if (!Value.starts_with("v2.11.0\t") || ++Wails != 1)
        throw Error("wails_unsupported_framework_declaration");
      PreviousWails = true;
    }
    if (Line.starts_with("build\tGOARCH=")) {
      if (Line != "build\tGOARCH=amd64" || ++Arch != 1)
        throw Error("wails_conflicting_build_declaration");
    }
    if (Line.starts_with("build\tGOOS=")) {
      if (Line != "build\tGOOS=" + std::string(Platform) || ++OS != 1)
        throw Error("wails_conflicting_build_declaration");
    }
  }
  if (Wails != 1 || Arch != 1 || OS != 1)
    throw Error("wails_required_build_declaration_missing");
}
} // namespace

void admitBuildInfo(Reader &R, const ProfileSpec &Profile) {
  constexpr std::string_view Magic = "\xff Go buildinf:";
  std::optional<Range> Found;
  for (const auto &M : R.Mappings) {
    if (M.Flags != 6)
      continue;
    for (uint64_t Base = 0; Base < M.File.Size;) {
      const auto Count =
          std::min<uint64_t>(BlobTransferBytes, M.File.Size - Base);
      const auto Bytes = R.read(
          {M.File.Offset + Base, std::min(Count + 32, M.File.Size - Base)});
      for (uint64_t I = 0; I < Count && I + 14 <= Bytes.size(); ++I) {
        R.step();
        if ((M.Virtual.Offset + Base + I) % 16 || Bytes.compare(I, 14, Magic))
          continue;
        if (Found)
          throw Error("wails_duplicate_build_info");
        const Range Header{M.File.Offset + Base + I, 32};
        if (!contains(M.File, Header))
          throw Error("wails_invalid_build_info");
        const auto H = R.read(Header);
        if (uint8_t(H[14]) != 8 || uint8_t(H[15]) != 2 ||
            std::any_of(H.begin() + 16, H.end(), [](char C) { return C; }))
          throw Error("wails_unsupported_build_info_layout");
        auto At = Header.end();
        if (inlineString(R, At, M.File) != "go1.25.7")
          throw Error("wails_unsupported_go_declaration");
        declarations(inlineString(R, At, M.File), Profile.Platform);
        Found = {Header.Offset, At - Header.Offset};
      }
      Base += Count;
    }
  }
  if (!Found)
    throw Error("wails_build_info_not_found");
  R.Evidence.BuildInfoOffset = Found->Offset;
  R.Evidence.BuildInfoSize = Found->Size;
  R.PrivateRanges.push_back(*Found);
}
} // namespace neverd::web::wails_detail
