//===- SymbolStore.cpp - Where the PDB a PE names is kept -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Search paths in _NT_SYMBOL_PATH syntax, symbol-store keys and the files
/// and URLs a PDB may be found at.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/SymbolStore.h"

#include "neverd/Common.h"
#include "neverd/debug/Limits.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

#include <algorithm>

namespace neverd {

namespace {

#define NEVERD_SYMBOL_STORE_STRING(Name, Value)                                \
  constexpr llvm::StringLiteral Name(Value);
#include "neverd/debug/SymbolStore.def"

/// Base name of a path a linker recorded inside the image.  Such a path is
/// spelled for the machine that produced the binary, so it may use Windows
/// separators no matter which host NeverD runs on.
llvm::StringRef recordedFileName(llvm::StringRef Recorded) {
  size_t Pos = Recorded.find_last_of("/\\");
  return Pos == llvm::StringRef::npos ? Recorded : Recorded.substr(Pos + 1);
}

std::filesystem::path pathOf(llvm::StringRef Text) {
  return std::filesystem::u8path(Text.str());
}

bool isServer(llvm::StringRef Store) {
  return Store.starts_with_insensitive(HTTPScheme) ||
         Store.starts_with_insensitive(HTTPSScheme);
}

void addCandidate(std::vector<PDBCandidate> &Out, std::filesystem::path Path,
                  PDBLocation Location) {
  if (Path.empty())
    return;
  if (std::none_of(Out.begin(), Out.end(), [&](const PDBCandidate &Known) {
        return Known.Path == Path;
      }))
    Out.push_back({std::move(Path), Location});
}

/// \p Text with every byte but RFC 3986's unreserved characters
/// percent-encoded, so that it is one segment of a URL's path.
std::string urlSegment(llvm::StringRef Text) {
  std::string Out;
  for (unsigned char C : Text) {
    if (llvm::isAlnum(C) || C == '-' || C == '.' || C == '_' || C == '~') {
      Out += static_cast<char>(C);
      continue;
    }
    Out += '%';
    Out += llvm::hexdigit(C >> 4, /*LowerCase=*/false);
    Out += llvm::hexdigit(C & 0xF, /*LowerCase=*/false);
  }
  return Out;
}

} // namespace

std::optional<PDBSymbolRequest> pdbSymbolRequest(const BinaryImage &Img) {
  const auto &DI = Img.DynInfo;
  if (!Img.isCOFF() ||
      DI.CodeViewPDBIdentityState != PDBIdentityState::Unique ||
      !DI.CodeViewPDBIdentity || !DI.CodeViewPDBIdentity->isValid())
    return std::nullopt;
  const auto Name = recordedFileName(DI.PDBPath);
  // The recorded directory is never a network or cache destination. Restrict
  // the basename to portable file characters before publishing it to clients.
  if (Name.empty() ||
      Name.size() >
          ::neverd::limits::debug::kSymbolStorePdbSymbolRequestNameSizeLimit ||
      Name.front() == '.' || !Name.ends_with_insensitive(".pdb"))
    return std::nullopt;
  for (unsigned char C : Name)
    if (C < 0x20 || C >= 0x7f || llvm::StringRef("<>:\"/\\|?*").contains(C))
      return std::nullopt;
  const auto &Identity = *DI.CodeViewPDBIdentity;
  std::string Key;
  if (Identity.Kind == PDBIdentityKind::RSDS) {
    // CodeView serializes GUID's first three fields little-endian. Symbol
    // stores spell those fields as a GUID, without braces or separators.
    constexpr unsigned Order[] = {3, 2, 1,  0,  5,  4,  7,  6,
                                  8, 9, 10, 11, 12, 13, 14, 15};
    for (unsigned I : Order)
      Key += llvm::utohexstr(Identity.Guid[I], false, 2);
  } else {
    Key = llvm::utohexstr(Identity.Signature, false, 8);
  }
  Key += llvm::utohexstr(Identity.Age, true);
  return PDBSymbolRequest{Name.lower(), std::move(Key)};
}

SymbolPath parseSymbolPath(llvm::StringRef Text,
                           const std::filesystem::path &DefaultCache) {
  SymbolPath Out;
  // The store a "cache*" element names keeps what the servers after it
  // serve, unless they name one of their own.
  std::filesystem::path Cache = DefaultCache;
  const auto StoreOf = [&](llvm::StringRef Store) {
    Store = Store.trim();
    return Store.empty() ? Cache : pathOf(Store);
  };
  const auto AddStore = [&](std::filesystem::path Store) {
    if (!Store.empty() && std::find(Out.Stores.begin(), Out.Stores.end(),
                                    Store) == Out.Stores.end())
      Out.Stores.push_back(std::move(Store));
  };

  llvm::SmallVector<llvm::StringRef, 8> Elements;
  Text.split(Elements, ElementSeparators, /*MaxSplit=*/-1,
             /*KeepEmpty=*/false);
  for (llvm::StringRef Element : Elements) {
    Element = Element.trim();
    if (Element.empty())
      continue;
    llvm::SmallVector<llvm::StringRef, 4> Parts;
    Element.split(Parts, StoreSeparator);
    const llvm::StringRef Head = Parts.front().trim();
    if (Parts.size() > 1 && Head.equals_insensitive(CachePrefix)) {
      Cache = StoreOf(Parts[1]);
      AddStore(Cache);
      continue;
    }
    size_t First = 0;
    if (Parts.size() > 1 && Head.equals_insensitive(ServerPrefix))
      First = 1;
    else if (Parts.size() > 2 && Head.equals_insensitive(SymsrvPrefix))
      First = 2;
    if (First == 0) {
      Out.Directories.push_back(pathOf(Element));
      continue;
    }
    llvm::ArrayRef<llvm::StringRef> Stores =
        llvm::ArrayRef(Parts).drop_front(First);
    // "srv*" names the public server behind the default store, as WinDbg's
    // .symfix spells it.
    if (Stores.size() == 1 && Stores.front().trim().empty()) {
      AddStore(Cache);
      Out.Servers.push_back({MicrosoftServer.str(), Cache});
      continue;
    }
    const llvm::StringRef Last = Stores.back().trim();
    if (!isServer(Last)) {
      for (llvm::StringRef Store : Stores)
        AddStore(StoreOf(Store));
      continue;
    }
    // The stores before a server keep what it serves, the nearest first.
    llvm::ArrayRef<llvm::StringRef> Downstream = Stores.drop_back();
    for (llvm::StringRef Store : Downstream)
      AddStore(StoreOf(Store));
    const std::filesystem::path Keeps =
        Downstream.empty() ? Cache : StoreOf(Downstream.front());
    AddStore(Keeps);
    Out.Servers.push_back({Last.str(), Keeps});
  }
  return Out;
}

std::filesystem::path defaultSymbolCache() {
  llvm::SmallString<256> Path;
  if (!llvm::sys::path::cache_directory(Path))
    return {};
  llvm::sys::path::append(Path, VendorDirectory, CacheDirectory);
  return pathOf(Path);
}

SymbolPath symbolPath(const std::optional<std::string> &Override) {
  const std::filesystem::path Cache = defaultSymbolCache();
  std::string Text;
  if (Override) {
    Text = *Override;
  } else {
    // The alternate path comes first, as Microsoft's debuggers search it.
    for (llvm::StringRef Variable : {AlternatePathVariable, PathVariable})
      if (std::optional<std::string> Value =
              llvm::sys::Process::GetEnv(Variable);
          Value && !llvm::StringRef(*Value).trim().empty()) {
        if (!Text.empty())
          Text += ElementSeparators.front();
        Text += *Value;
      }
    if (Text.empty()) {
      SymbolPath Default;
      if (!Cache.empty()) {
        Default.Stores.push_back(Cache);
        Default.Servers.push_back({MicrosoftServer.str(), Cache});
      }
      return Default;
    }
  }
  return parseSymbolPath(Text, Cache);
}

const char *pdbLocationName(PDBLocation Location) {
  switch (Location) {
  case PDBLocation::Recorded:
    return "recorded";
  case PDBLocation::Beside:
    return "beside";
  case PDBLocation::Directory:
    return "directory";
  case PDBLocation::Store:
    return "store";
  }
  return "store";
}

std::vector<PDBCandidate> pdbCandidates(const std::filesystem::path &BinaryPath,
                                        const BinaryImage &Img,
                                        const SymbolPath &Path) {
  std::vector<PDBCandidate> Out;
  const std::filesystem::path Dir = BinaryPath.parent_path();

  // The CodeView debug directory records the path the linker wrote the PDB to.
  // It resolves on the machine that produced the binary; anywhere else only
  // the file name survives the trip, which is why that name is also retried
  // next to the image and in the search path.
  llvm::StringRef Recorded(Img.DynInfo.PDBPath);
  if (Recorded.starts_with(kBuildIdPrefix))
    Recorded = {};
  const llvm::StringRef Name = recordedFileName(Recorded);
  if (!Recorded.empty()) {
    // A bare name, as Microsoft's builds record, names no place of its own:
    // read against the working directory it would find whatever happens to
    // be there.
    if (const std::filesystem::path Path = pathOf(Recorded); Path.is_absolute())
      addCandidate(Out, Path, PDBLocation::Recorded);
    addCandidate(Out, Dir / pathOf(Name), PDBLocation::Beside);
  }
  auto Companion = BinaryPath;
  Companion.replace_extension(".pdb");
  addCandidate(Out, std::move(Companion), PDBLocation::Beside);

  if (!Name.empty())
    for (const std::filesystem::path &Directory : Path.Directories)
      addCandidate(Out, Directory / pathOf(Name), PDBLocation::Directory);
  if (const auto Request = pdbSymbolRequest(Img))
    for (const std::filesystem::path &Store : Path.Stores)
      addCandidate(Out, symbolStorePath(Store, *Request), PDBLocation::Store);
  return Out;
}

std::filesystem::path symbolStorePath(const std::filesystem::path &Store,
                                      const PDBSymbolRequest &Request) {
  const std::filesystem::path Name = pathOf(Request.FileName);
  return Store / Name / pathOf(Request.Key) / Name;
}

std::string symbolServerURL(const SymbolServer &Server,
                            const PDBSymbolRequest &Request) {
  const std::string Name = urlSegment(Request.FileName);
  std::string URL = llvm::StringRef(Server.URL).rtrim('/').str();
  URL += '/';
  URL += Name;
  URL += '/';
  URL += urlSegment(Request.Key);
  URL += '/';
  URL += Name;
  return URL;
}

} // namespace neverd
