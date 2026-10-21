//===- Reader.cpp - Explicit Tauri PHF asset admission -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify every lookup and encoded asset before atomically sealing output.
/// No strings are carved, input run, or runtime Rust representation inferred.
///
//===----------------------------------------------------------------------===//

#include "../../BlobStore.h"
#include "../../PathPolicy.h"
#include "../../archives/PathIndex.h"
#include "../../compression/Brotli.h"
#include "PHF.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"
#include "neverd/web/Tauri.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>

namespace neverd::web {
namespace {
bool within(uint64_t At, uint64_t Size, uint64_t End) {
  return At <= End && Size <= End - At;
}
uint64_t number(std::string_view Bytes, uint64_t At, unsigned Width) {
  if (!within(At, Width, Bytes.size()))
    throw Error("tauri_invalid_field_range");
  uint64_t Out = 0;
  for (unsigned I = 0; I < Width; ++I)
    Out |= uint64_t(uint8_t(Bytes[At + I])) << (8 * I);
  return Out;
}
struct Reader {
  const Artifact &Input;
  TauriExtraction &E;
  const TauriLimits &Limits;
  struct Range {
    uint64_t Offset, Size;
    bool Payload;
  };
  std::map<uint64_t, Range> Ranges;
  void step(uint64_t Count = 1) {
    if (Count > Limits.Steps - E.Steps)
      throw Error("tauri_work_budget_exceeded");
    E.Steps += Count;
  }
  uint64_t mapped(uint64_t Address, uint64_t Size) {
    std::optional<uint64_t> Out;
    for (const auto &M : E.Initializer.Mappings) {
      step();
      if (Address < M.Address || Address - M.Address >= M.StoredSize ||
          !within(Address - M.Address, Size, M.StoredSize))
        continue;
      if (Out || !M.Readable)
        throw Error("tauri_ambiguous_mapping");
      Out = M.Offset + Address - M.Address;
    }
    if (!Out)
      throw Error("tauri_non_file_backed_pointer");
    return *Out;
  }
  void admitMappings() {
    const auto &Maps = E.Initializer.Mappings;
    if (Maps.empty() ||
        Maps.size() > ::neverd::limits::web::kReaderAdmitMappingsMapsSizeLimit)
      throw Error("tauri_mapping_budget_exceeded");
    for (size_t I = 0; I < Maps.size(); ++I) {
      const auto &M = Maps[I];
      if (!M.StoredSize || M.StoredSize > M.Size ||
          !within(M.Address, M.Size, UINT64_MAX) ||
          !within(M.Offset, M.StoredSize, Input.Content.size()))
        throw Error("tauri_invalid_mapping");
      for (size_t J = 0; J < I; ++J) {
        step();
        const auto &N = Maps[J];
        if ((M.Address < N.Address + N.Size &&
             N.Address < M.Address + M.Size) ||
            (M.Offset < N.Offset + N.StoredSize &&
             N.Offset < M.Offset + M.StoredSize))
          throw Error("tauri_ambiguous_mapping");
      }
    }
  }
  void range(uint64_t At, uint64_t Size, bool Payload = false) {
    if (!Size)
      return;
    step();
    const auto Next = Ranges.lower_bound(At);
    if (Next != Ranges.end() && At + Size > Next->first) {
      const auto &Other = Next->second;
      if (Payload && Other.Payload && At == Other.Offset && Size == Other.Size)
        return;
      throw Error("tauri_overlapping_evidence_ranges");
    }
    if (Next != Ranges.begin()) {
      const auto &Previous = std::prev(Next)->second;
      if (Previous.Offset + Previous.Size > At)
        throw Error("tauri_overlapping_evidence_ranges");
    }
    Ranges.emplace(At, Range{At, Size, Payload});
  }
};
} // namespace

bool tauriAvailable() {
  return archivePathsAvailable() && compression::brotliAvailable();
}
TauriExtraction extractTauri(const Artifact &Input,
                             const TauriInitializer &Init,
                             std::string_view Profile, TauriLimits Limits) {
  if (Profile != TauriProfile)
    throw Error("tauri_unsupported_profile");
  if (!tauriAvailable())
    throw Error("tauri_capability_unavailable");
  const TauriLimits D;
  if (Limits.Members > D.Members || Limits.StoredBytes > D.StoredBytes ||
      Limits.DecodedBytes > D.DecodedBytes ||
      Limits.MemberDecodedBytes > D.MemberDecodedBytes ||
      Limits.PrivateBytes > D.PrivateBytes || Limits.Steps > D.Steps ||
      Limits.CodecCalls > D.CodecCalls || Limits.CodecMillis > D.CodecMillis ||
      Limits.CodecMemory > D.CodecMemory)
    throw Error("tauri_invalid_limits");
  if (Input.Directory || Input.Content.size() > MaxTauriInputBytes ||
      Input.BlobHash != Init.InputHash ||
      Input.Content.digest() != Init.InputHash)
    throw Error("tauri_input_identity_mismatch");
  const auto &F = Init.Fields;
  if (!F[0].ExactAddress || !F[2].ExactAddress || F[1].ExactAddress ||
      F[3].ExactAddress || F[4].ExactAddress || !F[1].Value || !F[3].Value ||
      F[1].Value > Limits.Members || F[3].Value > Limits.Members)
    throw Error("tauri_invalid_initializer_fields");
  if (Init.End <= Init.Start ||
      Init.End - Init.Start >
          ::neverd::limits::web::kMaxTauriInitializerBytes ||
      Init.Instructions >
          ::neverd::limits::web::kReaderExtractTauriInitInstructionsLimit ||
      !Init.Instructions || Init.Function > Init.Start ||
      Init.Start - Init.Function >
          ::neverd::limits::web::kMaxTauriInitializerDistance ||
      !within(Init.Offset, Init.End - Init.Start, Input.Content.size()))
    throw Error("tauri_invalid_initializer_range");
  for (size_t I = 0; I < F.size(); ++I)
    if (F[I].Instruction < Init.Start || F[I].Instruction >= Init.End ||
        (I && F[I].Instruction <= F[I - 1].Instruction))
      throw Error("tauri_invalid_initializer_range");

  TauriExtraction E;
  E.ArtifactID = Input.ID;
  E.BlobHash = Input.BlobHash;
  E.Profile = Profile;
  E.Initializer = Init;
  E.ID = identity("tauri-extraction", {Input.ID, Input.BlobHash, Profile,
                                       std::to_string(Init.Start)});
  Reader R{Input, E, Limits, {}};
  R.admitMappings();
  if (R.mapped(Init.Start, Init.End - Init.Start) != Init.Offset)
    throw Error("tauri_initializer_mapping_mismatch");
  E.DisplacementsOffset = R.mapped(F[0].Value, F[1].Value * 8);
  E.EntriesOffset = R.mapped(F[2].Value, F[3].Value * 32);
  R.range(Init.Offset, Init.End - Init.Start);
  R.range(E.DisplacementsOffset, F[1].Value * 8);
  R.range(E.EntriesOffset, F[3].Value * 32);
  const auto Disps = Input.Content.read(E.DisplacementsOffset, F[1].Value * 8);
  const auto Rows = Input.Content.read(E.EntriesOffset, F[3].Value * 32);
  ArchivePathIndex Paths(E.PrivateBytes);
  BlobStore Decoded(Limits.DecodedBytes ? Limits.DecodedBytes : 1);
  const auto Deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(Limits.CodecMillis);
  for (uint64_t I = 0; I < F[3].Value; ++I) {
    R.step();
    const auto At = I * 32;
    const auto KeySize = number(Rows, At + 8, 8);
    const auto StoredSize = number(Rows, At + 24, 8);
    if (KeySize < 2 ||
        KeySize > ::neverd::limits::web::kReaderExtractTauriKeySizeLimit ||
        StoredSize > MaxBlobReadBytes ||
        StoredSize > Limits.StoredBytes - E.StoredBytes)
      throw Error("tauri_member_budget_exceeded");
    TauriMember M;
    M.RowOffset = E.EntriesOffset + At;
    M.NameOffset = R.mapped(number(Rows, At, 8), KeySize);
    R.range(M.NameOffset, KeySize);
    const auto Key = Input.Content.read(M.NameOffset, KeySize);
    R.step(Key.size());
    if (Key[0] != '/' || Key[1] == '/' || Key.back() == '/')
      throw Error("tauri_invalid_asset_key");
    M.Path = Key.substr(1);
    Paths.admit(M.Path, false);
    if (E.PrivateBytes > Limits.PrivateBytes)
      throw Error("tauri_metadata_budget_exceeded");
    const auto H = tauri_detail::phfHash(Key, F[4].Value);
    const auto Disp = uint64_t(H.G % F[1].Value) * 8;
    const auto Index = tauri_detail::phfIndex(
        H, number(Disps, Disp, 4), number(Disps, Disp + 4, 4), F[3].Value);
    if (Index != I)
      throw Error("tauri_phf_lookup_mismatch");
    M.Offset = R.mapped(number(Rows, At + 16, 8), StoredSize);
    R.range(M.Offset, StoredSize, true);
    M.Stored = Input.Content.slice(M.Offset, StoredSize);
    M.StoredHash = M.Stored.digest();
    const auto Bytes = compression::decodeBrotli(
        M.Stored.read(0, StoredSize),
        std::min(Limits.MemberDecodedBytes,
                 Limits.DecodedBytes - E.DecodedBytes),
        Limits.CodecMemory, E.CodecCalls, Limits.CodecCalls, Deadline);
    M.BlobHash = sha256(Bytes);
    M.DecodedSize = Bytes.size();
    M.ID = identity("tauri-member",
                    {E.ID, std::to_string(I), M.StoredHash, M.BlobHash});
    const auto Dot = M.Path.rfind('.');
    const auto Ext = Dot == std::string::npos
                         ? std::string_view{}
                         : std::string_view(M.Path).substr(Dot);
    M.Kind = Ext == ".js" || Ext == ".mjs" ? "javascript"
             : Ext == ".html"              ? "html"
             : Ext == ".css"               ? "css"
                                           : "asset";
    Decoded.append(Bytes);
    E.StoredBytes += StoredSize;
    E.DecodedBytes += Bytes.size();
    E.Members.push_back(std::move(M));
  }
  Decoded.seal();
  uint64_t At = 0;
  for (auto &M : E.Members) {
    M.Content = Decoded.whole().slice(At, M.DecodedSize);
    At += M.DecodedSize;
  }
  return E;
}

Snapshot tauriNamespace(const TauriExtraction &E) {
  Snapshot S;
  S.ID = E.ID;
  Artifact Root;
  Root.ID = E.ID;
  Root.Directory = true;
  S.Artifacts.push_back(std::move(Root));
  for (const auto &M : E.Members) {
    S.Artifacts.push_back(
        {M.ID, M.BlobHash, E.ID, M.Path, M.Kind, M.Content, false});
    S.InputBytes += M.Content.size();
  }
  return S;
}
} // namespace neverd::web
