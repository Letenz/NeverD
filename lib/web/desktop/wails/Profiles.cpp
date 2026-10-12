//===- Profiles.cpp - Explicit stored Wails producer layouts --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One registry owns platform declarations, container selection and names.
/// Unqualified containers and toolchain versions never select a fallback.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

namespace neverd::web::wails_detail {
namespace {
constexpr ProfileSpec Profiles[] = {
    {WailsProfile, "elf", "linux", ContainerKind::ELF},
    {WailsPEProfile, "pe", "windows", ContainerKind::PE}};
} // namespace
const ProfileSpec &requireProfile(std::string_view Name) {
  for (const auto &P : Profiles)
    if (P.Name == Name)
      return P;
  throw Error("wails_unsupported_profile");
}
} // namespace neverd::web::wails_detail

namespace neverd::web {
std::vector<std::string_view> wailsProfiles() {
  std::vector<std::string_view> Out;
  for (const auto &P : wails_detail::Profiles)
    Out.push_back(P.Name);
  return Out;
}
} // namespace neverd::web
