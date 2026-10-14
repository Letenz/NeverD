//===- DataPack.cpp - Bounded Chromium resource pack reader -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent storage-format implementation; validates the complete index
/// before hashing or publishing any resource. See the pinned format reference
/// in docs/web-datapack-profile.md.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/DataPack.h"

#include "../Internal.h"

#include "llvm/Support/Endian.h"

#include <set>

namespace neverd::web {
DataPackExtraction extractDataPack(const Artifact &Input,
                                   std::string_view Profile) {
  if (Profile != DataPackProfile && Profile != WebViewDataPackProfile)
    throw Error("datapack_unsupported_profile");
  if (Input.Directory)
    throw Error("artifact_has_no_bytes");
  const auto Size = Input.Content.size();
  if (Size > MaxDataPackBytes)
    throw Error("datapack_byte_budget_exceeded");
  const bool Wide = Profile == WebViewDataPackProfile;
  const uint64_t HeaderBytes = Wide ? 16 : 12;
  const uint32_t IDBytes = Wide ? 4 : 2;
  const uint32_t EntryBytes = IDBytes + 4;
  const uint32_t AliasBytes = 2 * IDBytes;
  if (Size < HeaderBytes)
    throw Error("datapack_truncated_header");
  const auto Header = Input.Content.read(0, HeaderBytes);
  if (llvm::support::endian::read32le(Header.data()) != 5)
    throw Error("datapack_unsupported_version");
  if (llvm::support::endian::read32le(Header.data() + 4) > 2)
    throw Error("datapack_invalid_encoding_or_padding");
  const auto ReadID = [&](const char *At) -> uint32_t {
    return Wide ? llvm::support::endian::read32le(At)
                : llvm::support::endian::read16le(At);
  };
  const uint32_t Count = ReadID(Header.data() + 8);
  const uint32_t Aliases = ReadID(Header.data() + 8 + IDBytes);
  if (uint64_t(Count) + Aliases > MaxDataPackResources)
    throw Error("datapack_resource_budget_exceeded");
  const uint64_t TableBytes =
      uint64_t(EntryBytes) * (Count + 1) + uint64_t(AliasBytes) * Aliases;
  const uint64_t Payload = HeaderBytes + TableBytes;
  if (Payload > Size)
    throw Error("datapack_truncated_index");
  const auto Table = Input.Content.read(HeaderBytes, TableBytes);
  const auto ResourceID = [&](uint32_t I) {
    return ReadID(Table.data() + uint64_t(EntryBytes) * I);
  };
  const auto Offset = [&](uint32_t I) {
    return llvm::support::endian::read32le(Table.data() +
                                           uint64_t(EntryBytes) * I + IDBytes);
  };
  if (ResourceID(Count) != 0 || Offset(0) != Payload || Offset(Count) != Size)
    throw Error("datapack_invalid_payload_partition");
  std::set<uint32_t> IDs;
  for (uint32_t I = 0; I < Count; ++I) {
    const auto ID = ResourceID(I);
    if ((I && ID <= ResourceID(I - 1)) || !IDs.insert(ID).second)
      throw Error("datapack_resource_ids_not_unique_sorted");
    if (Offset(I) > Offset(I + 1) || Offset(I + 1) > Size)
      throw Error("datapack_invalid_resource_range");
  }
  const auto *AliasTable = Table.data() + uint64_t(EntryBytes) * (Count + 1);
  uint32_t PreviousAlias = 0;
  for (uint32_t I = 0; I < Aliases; ++I) {
    const auto ID = ReadID(AliasTable + uint64_t(AliasBytes) * I);
    const auto Index = ReadID(AliasTable + uint64_t(AliasBytes) * I + IDBytes);
    if ((I && ID <= PreviousAlias) || !IDs.insert(ID).second)
      throw Error("datapack_alias_ids_not_unique_sorted");
    if (Index >= Count)
      throw Error("datapack_alias_index_out_of_range");
    PreviousAlias = ID;
  }
  if (Input.Content.digest() != Input.BlobHash)
    throw Error("datapack_input_hash_mismatch");
  DataPackExtraction E;
  E.ID = identity("datapack-extraction", {Profile, Input.ID, Input.BlobHash});
  E.ArtifactID = Input.ID;
  E.BlobHash = Input.BlobHash;
  E.Profile = Profile;
  E.Encoding = uint8_t(Header[4]);
  E.ResourceCount = Count;
  E.AliasCount = Aliases;
  E.IndexBytes = Payload;
  E.PayloadBytes = Size - Payload;
  E.Resources.reserve(Count + Aliases);
  for (uint32_t I = 0; I < Count; ++I) {
    DataPackResource R;
    R.ResourceID = ResourceID(I);
    R.CanonicalIndex = I;
    R.Offset = Offset(I);
    R.Content = Input.Content.slice(R.Offset, Offset(I + 1) - R.Offset);
    R.BlobHash = R.Content.digest();
    if (R.Content.size() >= 3)
      R.GzipCandidate = R.Content.read(0, 3) == std::string("\x1f\x8b\x08", 3);
    R.ID = identity("datapack-resource",
                    {E.ID, std::to_string(R.ResourceID), R.BlobHash});
    E.Resources.push_back(std::move(R));
  }
  for (uint32_t I = 0; I < Aliases; ++I) {
    const auto ID = ReadID(AliasTable + uint64_t(AliasBytes) * I);
    const auto Index = ReadID(AliasTable + uint64_t(AliasBytes) * I + IDBytes);
    auto R = E.Resources[Index];
    R.ResourceID = ID;
    R.Alias = true;
    R.ID =
        identity("datapack-resource", {E.ID, std::to_string(ID), R.BlobHash});
    E.Resources.push_back(std::move(R));
  }
  return E;
}
} // namespace neverd::web
