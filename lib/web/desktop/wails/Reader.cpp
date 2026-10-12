//===- Reader.cpp - Offline Wails extraction transaction -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own layout selection and atomic publication of immutable resource evidence.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include "../../PathPolicy.h"

namespace neverd::web::wails_detail {
uint64_t number(std::string_view Bytes, uint64_t At, unsigned Width) {
  within({At, Width}, Bytes.size());
  uint64_t Value = 0;
  for (unsigned I = 0; I < Width; ++I)
    Value |= uint64_t(uint8_t(Bytes[At + I])) << (8 * I);
  return Value;
}
void Reader::step(uint64_t Count) {
  if (Count > Limits.Steps - Evidence.Steps)
    throw Error("wails_work_budget_exceeded");
  Evidence.Steps += Count;
}
void Reader::metadata(uint64_t Count) {
  if (Count > Limits.PrivateBytes - Evidence.PrivateBytes)
    throw Error("wails_metadata_budget_exceeded");
  Evidence.PrivateBytes += Count;
}
std::string Reader::read(Range R) {
  within(R, Input.Content.size());
  if (R.Size > MaxWailsInputBytes + 8 * 1024 * 1024 - Evidence.ScannedBytes)
    throw Error("wails_scan_budget_exceeded");
  Evidence.ScannedBytes += R.Size;
  return Input.Content.read(R.Offset, R.Size);
}
} // namespace neverd::web::wails_detail

namespace neverd::web {
bool wailsAvailable() { return archivePathsAvailable(); }

WailsExtraction extractWails(const Artifact &Input, std::string_view Profile,
                             WailsLimits Limits) {
  const auto &Producer = wails_detail::requireProfile(Profile);
  if (!wailsAvailable())
    throw Error("wails_path_policy_unavailable");
  const WailsLimits Default;
  if (Limits.Tables > Default.Tables || Limits.Members > Default.Members ||
      Limits.Steps > Default.Steps ||
      Limits.PayloadBytes > Default.PayloadBytes ||
      Limits.PrivateBytes > Default.PrivateBytes)
    throw Error("wails_invalid_limits");
  if (Input.Directory || Input.Content.size() > MaxWailsInputBytes)
    throw Error("wails_invalid_input");
  WailsExtraction E;
  E.ArtifactID = Input.ID;
  E.BlobHash = Input.BlobHash;
  E.Profile = Producer.Name;
  E.ContainerFormat = Producer.ContainerFormat;
  E.Platform = Producer.Platform;
  E.Architecture = "x64";
  E.ID = identity("wails-extraction", {Input.ID, Input.BlobHash, Profile});
  wails_detail::Reader R{Input, E, Limits, {}, {}, {}};
  switch (Producer.Container) {
  case wails_detail::ContainerKind::ELF:
    wails_detail::admitELF(R);
    break;
  case wails_detail::ContainerKind::PE:
    wails_detail::admitPE(R);
    break;
  }
  wails_detail::admitBuildInfo(R, Producer);
  wails_detail::readTables(R);
  wails_detail::verifyContents(R);
  if (Input.Content.digest() != E.BlobHash)
    throw Error("wails_container_hash_mismatch");
  E.HashedBytes += Input.Content.size();
  return E;
}

Snapshot wailsNamespace(const WailsExtraction &E, uint32_t Table) {
  if (Table >= E.Tables.size())
    throw Error("wails_unknown_table");
  const auto &T = E.Tables[Table];
  Snapshot S;
  S.ID = T.ID;
  Artifact Root;
  Root.ID = T.ID;
  Root.Directory = true;
  S.Artifacts.push_back(std::move(Root));
  for (uint32_t I = T.FirstMember; I < T.FirstMember + T.MemberCount; ++I) {
    const auto &M = E.Members.at(I);
    S.Artifacts.push_back(
        {M.ID, M.BlobHash, T.ID, M.Path, M.Kind, M.Content, M.Directory});
    S.InputBytes += M.Content.size();
  }
  return S;
}
} // namespace neverd::web
