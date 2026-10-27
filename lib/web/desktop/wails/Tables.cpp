//===- Tables.cpp - Stored Go embed.FS table admission --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Complete self-referential slice tables, Go directory ordering and private
/// portable namespaces. No symbol-name heuristic chooses a runtime filesystem.
///
//===----------------------------------------------------------------------===//

#include "../../Internal.h"
#include "../../archives/PathIndex.h"
#include "Reader.h"

#include "neverd/web/Limits.h"

#include <algorithm>
#include <map>

namespace neverd::web::wails_detail {
namespace {
std::pair<std::string_view, std::string_view> order(std::string_view Path) {
  const auto At = Path.rfind('/');
  return At == std::string_view::npos
             ? std::pair<std::string_view, std::string_view>{".", Path}
             : std::pair{Path.substr(0, At), Path.substr(At + 1)};
}
const char *kind(std::string_view Path) {
  if (Path.ends_with(".js") || Path.ends_with(".mjs") || Path.ends_with(".cjs"))
    return "javascript";
  if (Path.ends_with(".html") || Path.ends_with(".htm"))
    return "html";
  if (Path.ends_with(".css"))
    return "css";
  if (Path.ends_with(".map"))
    return "source_map_candidate";
  return "stored_asset";
}
void table(Reader &R, Range Header, uint64_t Count) {
  auto &E = R.Evidence;
  if (E.Tables.size() >= R.Limits.Tables ||
      Count > R.Limits.Members - E.Members.size())
    throw Error("wails_record_budget_exceeded");
  WailsTable T;
  T.Offset = Header.Offset;
  T.Size = 24 + Count * 48;
  T.FirstMember = E.Members.size();
  T.MemberCount = Count;
  const auto Index = uint32_t(E.Tables.size());
  T.ID = identity("wails-embed-table", {E.ID, std::to_string(T.Offset)});
  const Range Whole{T.Offset, T.Size};
  for (const auto &Private : R.PrivateRanges) {
    R.step();
    if (overlaps(Whole, Private))
      throw Error("wails_overlapping_table_metadata");
  }
  const auto Rows = R.read({Header.end(), Count * 48});
  R.PrivateRanges.push_back(Whole);
  R.metadata(sizeof(WailsTable) + 128 + Count * 48);
  std::map<std::string, bool> Entries;
  uint64_t PathBytes = 0;
  ArchivePathIndex Paths(PathBytes);
  for (uint64_t I = 0; I < Count; ++I) {
    R.step();
    const auto At = I * 48;
    const auto NameSize = number(Rows, At + 8, 8);
    if (!NameSize ||
        NameSize > ::neverd::limits::web::kTablesTableNameSizeLimit)
      throw Error("wails_invalid_embed_name");
    const auto Name = R.mapped(number(Rows, At, 8), NameSize, true);
    auto Path = R.read(Name);
    if (!validUtf8(Path))
      throw Error("wails_invalid_embed_name");
    const bool Directory = Path.ends_with('/');
    if (Directory)
      Path.pop_back();
    if (I && !(order(E.Members.back().Path) < order(Path)))
      throw Error("wails_invalid_embed_order");
    const auto Before = PathBytes;
    Paths.admit(Path, Directory);
    R.metadata(PathBytes - Before + sizeof(WailsMember) + Path.size() + 224);
    Entries.emplace(Path, Directory);
    WailsMember M;
    M.Path = std::move(Path);
    M.Directory = Directory;
    M.Kind = Directory ? "directory" : kind(M.Path);
    M.Table = Index;
    M.TableID = T.ID;
    M.NameOffset = Name.Offset;
    M.ID = identity("wails-embed-member", {T.ID, std::to_string(I)});
    const auto Address = number(Rows, At + 16, 8);
    const auto Size = number(Rows, At + 24, 8);
    const auto Expected = Rows.substr(At + 32, 16);
    if (Directory) {
      if (Address || Size ||
          std::any_of(Expected.begin(), Expected.end(),
                      [](char C) { return C; }))
        throw Error("wails_invalid_directory_record");
    } else {
      if (Size > R.Limits.PayloadBytes - E.PayloadBytes)
        throw Error("wails_payload_budget_exceeded");
      const auto Data =
          !Size && !Address ? Range{} : R.mapped(Address, Size, true);
      M.Offset = Data.Offset;
      M.Content = R.Input.Content.slice(Data.Offset, Data.Size);
      E.PayloadBytes += Size;
    }
    R.ExpectedHashes.push_back(Expected);
    R.PrivateRanges.push_back(Name);
    E.Members.push_back(std::move(M));
  }
  // Go's compiler emits every ancestor directory. Implicit path admission
  // alone is insufficient to establish that this is the qualified table.
  for (const auto &[Path, Directory] : Entries) {
    for (auto Slash = Path.find('/'); Slash != std::string::npos;
         Slash = Path.find('/', Slash + 1)) {
      R.step();
      const auto Parent = Entries.find(Path.substr(0, Slash));
      if (Parent == Entries.end() || !Parent->second)
        throw Error("wails_missing_embed_directory");
    }
  }
  E.Tables.push_back(std::move(T));
}
} // namespace

void readTables(Reader &R) {
  for (const auto &M : R.Mappings) {
    if (M.Flags != 4)
      continue;
    for (uint64_t Base = 0; Base < M.File.Size;) {
      const auto Count =
          std::min<uint64_t>(BlobTransferBytes, M.File.Size - Base);
      const auto Bytes = R.read(
          {M.File.Offset + Base, std::min(Count + 40, M.File.Size - Base)});
      const auto First = (8 - (M.Virtual.Offset + Base) % 8) % 8;
      for (uint64_t I = First; I < Count && I + 40 <= Bytes.size(); I += 8) {
        R.step();
        const auto Address = M.Virtual.Offset + Base + I;
        if (number(Bytes, I, 8) != Address + 24)
          continue;
        const auto N = number(Bytes, I + 8, 8);
        // Go also emits self-referential slices for unrelated runtime type
        // metadata. A nonempty first name string is part of this profile's
        // recognition framing, before any slice is selected as a filesystem.
        // Incompatible shapes remain opaque in the original container; they
        // are neither decoded as assets nor claimed as complete coverage.
        const auto FirstNameSize = number(Bytes, I + 32, 8);
        if (!N || number(Bytes, I + 16, 8) != N || !FirstNameSize ||
            FirstNameSize >
                ::neverd::limits::web::kTablesReadTablesFirstNameSizeLimit)
          continue;
        if (N > R.Limits.Members)
          throw Error("wails_record_budget_exceeded");
        const auto Whole = R.mapped(Address, 24 + N * 48, true);
        table(R, {Whole.Offset, 24}, N);
      }
      Base += Count;
    }
  }
  if (R.Evidence.Tables.empty())
    throw Error("wails_embed_table_not_found");
}
} // namespace neverd::web::wails_detail
