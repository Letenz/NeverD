//===- Reader.cpp - Offline pkg partitions and store hashes -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Complete byte preservation with unique-range accounting and typed stores.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include "../PathPolicy.h"

#include "neverd/web/Error.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <map>

namespace neverd::web {
namespace pkg_detail {
void step(PkgExtraction &E, uint64_t Count) {
  if (Count > MaxPkgSteps - E.Steps)
    throw Error("pkg_work_budget_exceeded");
  E.Steps += Count;
}
void metadata(PkgExtraction &E, uint64_t Count) {
  if (E.PrivateBytes > MaxPkgPrivateBytes ||
      Count > MaxPkgPrivateBytes - E.PrivateBytes)
    throw Error("pkg_metadata_budget_exceeded");
  E.PrivateBytes += Count;
}

void hashStores(PkgExtraction &E) {
  using Range = std::pair<uint64_t, uint64_t>;
  std::map<Range, std::pair<PkgMember *, bool>> Ranges;
  for (auto &M : E.Members) {
    step(E);
    auto [It, Inserted] = Ranges.try_emplace(Range{M.Offset, M.Content.size()},
                                             &M, M.privateHash());
    if (Inserted)
      metadata(E, sizeof(Range) + 64);
    else
      It->second.second |= M.privateHash();
  }
  uint64_t End = E.PayloadOffset;
  for (const auto &[R, M] : Ranges) {
    step(E);
    if (!R.second)
      continue;
    if (R.first < End)
      throw Error("pkg_partial_store_overlap");
    End = R.first + R.second;
    E.ReferencedBytes += R.second;
  }
  E.UniqueStoreRanges = Ranges.size();
  for (const auto &[R, Record] : Ranges) {
    auto *M = Record.first;
    M->BlobHash = M->Content.digest();
    E.HashedBytes += R.second;
  }
  uint64_t Ordinal = 0;
  for (auto &M : E.Members) {
    const auto &RangeRecord = Ranges.at({M.Offset, M.Content.size()});
    M.BlobHash = RangeRecord.first->BlobHash;
    M.MetadataRange = RangeRecord.second;
    M.RangeID = identity("pkg-store-range", {E.ID, std::to_string(M.Offset),
                                             std::to_string(M.Content.size())});
    M.ID = identity("pkg-store", {E.ID, std::to_string(Ordinal++),
                                  pkgStoreKind(M.Store), M.RangeID});
  }
}
} // namespace pkg_detail

const char *pkgStoreKind(PkgStore Store) {
  switch (Store) {
  case PkgStore::Cache:
    return "opaque_v8_cache";
  case PkgStore::Content:
    return "stored_content";
  case PkgStore::DirectoryList:
    return "directory_list_metadata";
  case PkgStore::Stat:
    return "stat_metadata";
  }
  throw Error("pkg_invalid_store_kind");
}

bool pkgAvailable() { return archivePathsAvailable(); }

PkgExtraction extractPkg(const Artifact &Input, std::string_view Profile) {
  if (Profile != PkgProfile)
    throw Error("pkg_unsupported_profile");
  if (!pkgAvailable())
    throw Error("pkg_path_policy_unavailable");
  if (Input.Directory || Input.Content.size() > MaxPkgInputBytes)
    throw Error("pkg_invalid_input");
  PkgExtraction E;
  E.ArtifactID = Input.ID;
  E.BlobHash = Input.BlobHash;
  E.ID = identity("pkg-extraction", {Input.ID, Input.BlobHash, Profile});
  const auto Layout = pkg_detail::locate(Input, E);
  const auto Prelude =
      Input.Content.read(Layout.PreludeOffset, Layout.PreludeSize);
  E.ScannedBytes += Prelude.size();
  if (!validUtf8(Prelude))
    throw Error("pkg_invalid_prelude_utf8");
  pkg_detail::readIndex(Input, Prelude, E);
  auto Region = [&](const char *Kind, uint64_t Offset, uint64_t Size) {
    PkgRegion R;
    R.Kind = Kind;
    R.Offset = Offset;
    R.Content = Input.Content.slice(Offset, Size);
    R.ID = identity("pkg-region",
                    {E.ID, Kind, std::to_string(Offset), std::to_string(Size)});
    for (const auto &M : E.Members) {
      pkg_detail::step(E);
      if (M.privateHash() && M.Offset == Offset && M.Content.size() == Size)
        R.MetadataRange = true;
    }
    E.Regions.push_back(std::move(R));
  };
  Region("opaque_prefix", 0, E.PayloadOffset);
  Region("payload_storage", E.PayloadOffset, E.PayloadSize);
  Region("prelude_storage", Layout.PreludeOffset, Layout.PreludeSize);
  const auto Trailer = Layout.PreludeOffset + Layout.PreludeSize;
  Region("opaque_trailer", Trailer, Input.Content.size() - Trailer);
  llvm::SHA256 Whole;
  for (auto &R : E.Regions) {
    llvm::SHA256 Hash;
    for (uint64_t At = 0; At < R.Content.size();) {
      const auto Count = std::min(BlobTransferBytes, R.Content.size() - At);
      const auto Bytes = R.Content.read(At, Count);
      Whole.update(Bytes);
      Hash.update(Bytes);
      At += Count;
      E.HashedBytes += Count;
    }
    R.BlobHash = llvm::toHex(Hash.final(), true);
  }
  if (llvm::toHex(Whole.final(), true) != E.BlobHash)
    throw Error("pkg_hash_mismatch");
  if (E.HashedBytes > 2 * Input.Content.size() ||
      E.ScannedBytes > 2 * Input.Content.size())
    throw Error("pkg_byte_work_budget_exceeded");
  return E;
}

Snapshot pkgNamespace(const PkgExtraction &E) {
  Snapshot S;
  S.ID = E.ID;
  Artifact Root;
  Root.ID = E.ID;
  Root.Directory = true;
  S.Artifacts.push_back(std::move(Root));
  for (const auto &M : E.Members) {
    if (M.Directory && M.Store == PkgStore::Stat && !M.Path.empty()) {
      S.Artifacts.push_back({M.ID, "", E.ID, M.Path, "directory", {}, true});
    } else if (M.selectable()) {
      S.Artifacts.push_back(
          {M.ID, M.BlobHash, E.ID, M.Path, "file", M.Content, false});
      S.InputBytes += M.Content.size();
    }
  }
  return S;
}
} // namespace neverd::web
