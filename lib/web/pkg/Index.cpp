//===- Index.cpp - Declarative pkg stores and namespace -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed prelude framing, bounded JSON and private virtual path admission.
///
//===----------------------------------------------------------------------===//

#include "../JsonReader.h"
#include "../PathPolicy.h"
#include "../archives/PathIndex.h"
#include "Reader.h"

#include "neverd/web/Error.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace neverd::web::pkg_detail {
namespace {
llvm::json::Value parse(std::string_view Text, PkgExtraction &E,
                        uint64_t MaxBytes = MaxPkgIndexBytes) {
  uint64_t Nodes = 0;
  auto V = parseBoundedJSON(
      Text,
      {MaxBytes, MaxPkgJSONDepth, MaxPkgJSONNodes - E.JSONNodes, MaxBytes},
      &Nodes);
  E.JSONNodes += Nodes;
  step(E, Text.size() + Nodes);
  return V;
}

std::array<llvm::json::Value, 5> arguments(std::string_view Text,
                                           PkgExtraction &E) {
  constexpr std::string_view Prefix =
      "(function(process, require, console, EXECPATH_FD, PAYLOAD_POSITION, "
      "PAYLOAD_SIZE) { return (function (REQUIRE_COMMON, VIRTUAL_FILESYSTEM, "
      "DEFAULT_ENTRYPOINT, SYMLINKS, DICT, DOCOMPRESS) {\n";
  constexpr std::string_view Suffix = "\n);\n})";
  constexpr std::string_view Boundary = "\n},\n";
  if (!Text.starts_with(Prefix) || !Text.ends_with(Suffix))
    throw Error("pkg_unsupported_prelude");
  const auto Begin = Text.rfind(Boundary);
  if (Begin == Text.npos || Begin < Prefix.size() ||
      Text.substr(Prefix.size(), Begin - Prefix.size())
              .find("\n})(function (exports) {\n") == Text.npos)
    throw Error("pkg_unsupported_prelude");
  const auto Start = Begin + Boundary.size();
  if (Start > Text.size() - Suffix.size())
    throw Error("pkg_invalid_index_framing");
  auto Tail = Text.substr(Start, Text.size() - Suffix.size() - Start);
  if (Tail.size() > MaxPkgIndexBytes)
    throw Error("pkg_index_budget_exceeded");
  std::array<llvm::json::Value, 5> Args{nullptr, nullptr, nullptr, nullptr,
                                        nullptr};
  for (size_t I = 0; I < Args.size(); ++I) {
    const auto End = Tail.find("\n,\n");
    if ((I + 1 == Args.size()) != (End == Tail.npos))
      throw Error("pkg_invalid_index_framing");
    const auto Row = Tail.substr(0, End);
    if (Row.find_first_of("\r\n") != Row.npos)
      throw Error("pkg_invalid_index_framing");
    Args[I] = parse(Row, E);
    if (End != Tail.npos)
      Tail.remove_prefix(End + 3);
  }
  return Args;
}

std::string path(std::string_view Raw, PkgExtraction &E) {
  constexpr std::string_view Posix = "/snapshot", Windows = "C:\\snapshot";
  bool Win = false;
  if (Raw == Posix || Raw.starts_with("/snapshot/")) {
    Raw.remove_prefix(Posix.size());
  } else if (Raw == Windows || Raw.starts_with("C:\\snapshot\\")) {
    Raw.remove_prefix(Windows.size());
    Win = true;
  } else {
    throw Error("pkg_unsupported_virtual_path");
  }
  const auto Style = Win ? "windows_snapshot" : "posix_snapshot";
  if (!E.PathStyle.empty() && E.PathStyle != Style)
    throw Error("pkg_mixed_virtual_roots");
  E.PathStyle = Style;
  if (Raw.size() == 1)
    throw Error("pkg_unsupported_virtual_path");
  if (!Raw.empty())
    Raw.remove_prefix(1);
  std::string Key(Raw);
  if (Win) {
    if (Key.find('/') != Key.npos)
      throw Error("pkg_unsupported_virtual_path");
    std::replace(Key.begin(), Key.end(), '\\', '/');
  }
  if (!Key.empty())
    (void)archivePathKey(Key);
  step(E, Key.size() + 1);
  return Key;
}

using Range = std::pair<uint64_t, uint64_t>;
struct Stat {
  bool Directory = false;
  uint64_t Size = 0;
};
Stat stat(const PkgMember &M, PkgExtraction &E) {
  if (M.Content.size() > MaxPkgStatBytes)
    throw Error("pkg_stat_budget_exceeded");
  const auto Bytes = M.Content.read(0, M.Content.size());
  E.ScannedBytes += Bytes.size();
  const auto V = parse(Bytes, E, MaxPkgStatBytes);
  const auto *O = V.getAsObject();
  if (!O)
    throw Error("pkg_invalid_stat");
  const auto File = O->getBoolean("isFileValue");
  const auto Directory = O->getBoolean("isDirectoryValue");
  const auto Socket = O->getBoolean("isSocketValue");
  const auto Link = O->getBoolean("isSymbolicLinkValue");
  const auto Size = O->getInteger("size");
  if (!File || !Directory || !Socket || !Link || !Size || *Size < 0 ||
      *File == *Directory || *Socket || *Link)
    throw Error("pkg_unsupported_stat_kind");
  return {*Directory, uint64_t(*Size)};
}

void admitMembers(PkgExtraction &E) {
  ArchivePathIndex Paths(E.PrivateBytes);
  std::map<Range, Stat> Stats;
  for (size_t I = 0; I < E.Members.size();) {
    const auto &Name = E.Members[I].Path;
    size_t End = I;
    uint8_t Stores = 0;
    const PkgMember *StatMember = nullptr, *Content = nullptr;
    while (End < E.Members.size() && E.Members[End].Path == Name) {
      const auto &M = E.Members[End++];
      Stores |= uint8_t(1U << uint8_t(M.Store));
      if (M.Store == PkgStore::Stat)
        StatMember = &M;
      if (M.Store == PkgStore::Content)
        Content = &M;
    }
    if (!StatMember)
      throw Error("pkg_missing_stat");
    const Range R{StatMember->Offset, StatMember->Content.size()};
    auto [It, Inserted] = Stats.try_emplace(R);
    if (Inserted)
      It->second = stat(*StatMember, E);
    const auto &Info = It->second;
    if ((Info.Directory && (Stores & 3)) || (!Info.Directory && (Stores & 4)))
      throw Error("pkg_conflicting_store_kind");
    if (Name.empty()) {
      if (!Info.Directory)
        throw Error("pkg_invalid_namespace_root");
    } else {
      Paths.admit(Name, Info.Directory);
      metadata(E, 0); // Shared path accounting has a larger independent bound.
    }
    ++E.PathCount;
    E.DirectoryCount += Info.Directory;
    E.ContentFileCount += Content != nullptr;
    E.CacheOnlyFileCount += !Info.Directory && !Content && (Stores & 1);
    E.UnavailableFileCount += !Info.Directory && !Content;
    for (auto J = I; J < End; ++J) {
      E.Members[J].Directory = Info.Directory;
      E.Members[J].CachePresent = Stores & 1;
      // pkg can preprocess source while retaining the original stat size.
      E.Members[J].DeclaredStatSize = Info.Size;
    }
    step(E, End - I);
    I = End;
  }
}

void links(const llvm::json::Object &Object, PkgExtraction &E) {
  if (Object.size() > MaxPkgLinks)
    throw Error("pkg_link_budget_exceeded");
  std::set<std::string> Keys;
  for (const auto &[Name, Value] : Object) {
    const auto Target = Value.getAsString();
    if (!Target)
      throw Error("pkg_invalid_link");
    PkgLink L;
    L.Path = path(Name.str(), E);
    L.Target = path(Target->str(), E);
    const auto Key = L.Path.empty() ? "" : archivePathKey(L.Path);
    if (!Keys.insert(Key).second)
      throw Error("pkg_link_path_collision");
    metadata(E, L.Path.size() + L.Target.size() + Key.size() + sizeof(PkgLink) +
                    64);
    E.Links.push_back(std::move(L));
  }
  std::sort(E.Links.begin(), E.Links.end(), [&](const auto &A, const auto &B) {
    step(E, 1 + std::min(A.Path.size(), B.Path.size()));
    return A.Path < B.Path;
  });
  uint64_t Ordinal = 0;
  for (auto &L : E.Links) {
    L.ID = identity("pkg-link", {E.ID, std::to_string(Ordinal++)});
    if (L.Path.empty() || E.DeclaredEntry == L.Path ||
        (E.DeclaredEntry.starts_with(L.Path) &&
         E.DeclaredEntry.size() > L.Path.size() &&
         E.DeclaredEntry[L.Path.size()] == '/'))
      E.EntrySymlinkPrefix = true;
  }
}
} // namespace

void readIndex(const Artifact &Input, std::string_view Prelude,
               PkgExtraction &E) {
  const auto Args = arguments(Prelude, E);
  const auto *VFS = Args[0].getAsObject();
  const auto Entry = Args[1].getAsString();
  const auto *Links = Args[2].getAsObject();
  const auto *Dictionary = Args[3].getAsObject();
  if (!VFS || !Entry || !Links || !Dictionary)
    throw Error("pkg_invalid_index");
  if (Args[4].getAsInteger() != 0 || !Dictionary->empty())
    throw Error("pkg_compression_unsupported");
  if (VFS->size() > MaxPkgPaths)
    throw Error("pkg_path_budget_exceeded");
  E.DeclaredEntry = path(Entry->str(), E);
  metadata(E, E.DeclaredEntry.size());
  for (const auto &[Name, Value] : *VFS) {
    const auto Key = path(Name.str(), E);
    const auto *Stores = Value.getAsObject();
    if (!Stores || Stores->empty() || Stores->size() > 4)
      throw Error("pkg_invalid_store_table");
    for (const auto &[Kind, PairValue] : *Stores) {
      const auto Store = Kind.str();
      const auto *Pair = PairValue.getAsArray();
      if (Store.size() != 1 || Store[0] < '0' || Store[0] > '3' || !Pair ||
          Pair->size() != 2)
        throw Error("pkg_invalid_store_range");
      const auto Offset = (*Pair)[0].getAsInteger();
      const auto Size = (*Pair)[1].getAsInteger();
      if (!Offset || !Size || *Offset < 0 || *Size < 0 ||
          uint64_t(*Offset) > E.PayloadSize ||
          uint64_t(*Size) > E.PayloadSize - uint64_t(*Offset))
        throw Error("pkg_invalid_store_range");
      if (E.Members.size() >= MaxPkgStores)
        throw Error("pkg_store_budget_exceeded");
      metadata(E, Key.size() + sizeof(PkgMember) + 3 * 64);
      PkgMember M;
      M.Path = Key;
      M.Store = PkgStore(Store[0] - '0');
      M.Offset = E.PayloadOffset + uint64_t(*Offset);
      M.Content = Input.Content.slice(M.Offset, uint64_t(*Size));
      E.Members.push_back(std::move(M));
      step(E);
    }
  }
  std::sort(E.Members.begin(), E.Members.end(),
            [&](const auto &A, const auto &B) {
              step(E, 1 + 2 * std::min(A.Path.size(), B.Path.size()));
              return A.Path == B.Path ? A.Store < B.Store : A.Path < B.Path;
            });
  // Range validation precedes stat reads; unique stat ranges are parsed once.
  // Aliases cannot amplify either payload hashing or metadata byte reads.
  hashStores(E);
  admitMembers(E);
  links(*Links, E);
  for (const auto &M : E.Members)
    if (M.Path == E.DeclaredEntry) {
      E.EntryCachePresent |= M.CachePresent;
      if (M.selectable())
        E.LiteralEntryMemberID = M.ID;
    }
}
} // namespace neverd::web::pkg_detail
