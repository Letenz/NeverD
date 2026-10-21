//===- ModuleImage.cpp - Owned immutable binary/debug observations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owned immutable binary/debug observations.
///

#include "neverd/modules/ModuleImage.h"

#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/modules/Limits.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

namespace neverd::modules {
namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "module input: " + Message);
}

/// The existing binary/debug providers are path based. Keep private owned
/// copies so their memory mappings and any deferred reads see the same input.
/// This is a lifetime adapter, not a second format or identity parser.
struct SourceSnapshot {
  std::filesystem::path Source, Root, Path;
  std::vector<uint8_t> Bytes;
  std::string Digest;

  ~SourceSnapshot() {
    if (Root.empty())
      return;
    std::error_code EC;
    if (!Path.empty())
      std::filesystem::permissions(Path, std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::add, EC);
    std::filesystem::remove_all(Root, EC);
  }

  static llvm::Expected<std::shared_ptr<SourceSnapshot>>
  read(const std::filesystem::path &Source, uint64_t Limit) {
    if (!Limit)
      return invalid("retained source-byte budget exhausted");
    std::error_code EC;
    if (!std::filesystem::is_regular_file(Source, EC) || EC)
      return invalid("not a readable regular file: " + pathToUTF8(Source));
    const auto Size = std::filesystem::file_size(Source, EC);
    if (EC || Size > Limit)
      return invalid("file exceeds source-byte limit: " + pathToUTF8(Source));
    std::ifstream Input(Source, std::ios::binary);
    if (!Input)
      return invalid("cannot open: " + pathToUTF8(Source));
    auto Snapshot = std::make_shared<SourceSnapshot>();
    Snapshot->Source = Source;
    std::array<char, 16384> Chunk;
    while (Input) {
      Input.read(Chunk.data(), Chunk.size());
      const auto Count = uint64_t(Input.gcount());
      if (Count > Limit - Snapshot->Bytes.size())
        return invalid("file grew beyond source-byte limit");
      Snapshot->Bytes.insert(Snapshot->Bytes.end(), Chunk.data(),
                             Chunk.data() + Count);
    }
    if (!Input.eof())
      return invalid("cannot read complete input: " + pathToUTF8(Source));
    if (Snapshot->Bytes.size() != Size)
      return invalid("file size changed while reading");
    Snapshot->Digest =
        llvm::toHex(llvm::ArrayRef(llvm::SHA256::hash(Snapshot->Bytes)), true);
    llvm::SmallString<128> Directory;
    if (auto Error =
            llvm::sys::fs::createUniqueDirectory("neverd-module", Directory))
      return llvm::createStringError(Error,
                                     "cannot create module source snapshot");
    const auto DirectoryText = Directory.str();
    Snapshot->Root = std::u8string(DirectoryText.begin(), DirectoryText.end());
    auto FileName = std::filesystem::path("input");
    FileName += Source.extension();
    Snapshot->Path = Snapshot->Root / FileName;
    std::ofstream Output(Snapshot->Path, std::ios::binary);
    if (!Output)
      return invalid("cannot create private source copy");
    Output.write(reinterpret_cast<const char *>(Snapshot->Bytes.data()),
                 std::streamsize(Snapshot->Bytes.size()));
    Output.close();
    if (!Output)
      return invalid("cannot finish private source copy");
    std::filesystem::permissions(Snapshot->Path,
                                 std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace, EC);
    if (EC)
      return llvm::createStringError(EC, "cannot protect private source copy");
    return Snapshot;
  }
};

llvm::Expected<std::shared_ptr<const DebugContext>>
loadDebug(const SourceSnapshot &Image, const BinaryImage &Parsed,
          const SourceSnapshot &Debug, DebugKind Kind) {
  DebugInfoRequest Request;
  switch (Kind) {
  case DebugKind::PDB:
    Request.PDBPath = Debug.Path;
    break;
  case DebugKind::DWARF:
    Request.DWARFPath = Debug.Path;
    break;
  case DebugKind::Map:
    Request.MapPath = Debug.Path;
    break;
  }
  auto Result = loadDebugInfo(Image.Path, Parsed, Request);
  if (!Result)
    return invalid(
        "debug file " + pathToUTF8(Debug.Source) + ": " +
        (Result.Error.empty() ? "no debug information" : Result.Error));
  return std::shared_ptr<const DebugContext>(std::move(Result.Context));
}
} // namespace

struct ModuleImage::Storage {
  std::shared_ptr<SourceSnapshot> ImageSource;
  std::shared_ptr<const BinaryImage> Image;
  std::shared_ptr<SourceSnapshot> DebugSource;
  // Destroy debug providers before their mapped source copies.
  std::shared_ptr<const DebugContext> Debug;
  std::optional<DebugKind> Kind;
  uint64_t SymbolLimit = 0;
};

ModuleImage::ModuleImage(std::unique_ptr<Storage> Data)
    : Data(std::move(Data)) {}
ModuleImage::~ModuleImage() = default;
const BinaryImage &ModuleImage::image() const { return *Data->Image; }
const DebugContext *ModuleImage::debug() const { return Data->Debug.get(); }
llvm::ArrayRef<uint8_t> ModuleImage::bytes() const {
  return Data->ImageSource->Bytes;
}
const std::string &ModuleImage::imageDigest() const {
  return Data->ImageSource->Digest;
}
const std::string &ModuleImage::debugDigest() const {
  static const std::string Empty;
  return Data->DebugSource ? Data->DebugSource->Digest : Empty;
}
const std::filesystem::path &ModuleImage::sourcePath() const {
  return Data->ImageSource->Source;
}
const std::filesystem::path &ModuleImage::debugSourcePath() const {
  static const std::filesystem::path Empty;
  return Data->DebugSource ? Data->DebugSource->Source : Empty;
}
const std::filesystem::path &ModuleImage::preparedImagePath() const {
  return Data->ImageSource->Path;
}
const std::filesystem::path &ModuleImage::preparedDebugPath() const {
  static const std::filesystem::path Empty;
  return Data->DebugSource ? Data->DebugSource->Path : Empty;
}
uint64_t ModuleImage::sourceBytes() const {
  return Data->ImageSource->Bytes.size() +
         (Data->DebugSource ? Data->DebugSource->Bytes.size() : 0);
}
std::optional<DebugKind> ModuleImage::debugKind() const { return Data->Kind; }
bool ModuleImage::hasAuthenticatedFunctionSignatures() const {
  return Data->Debug && Data->Debug->hasAuthenticatedFunctionSignatures();
}
bool ModuleImage::hasAuthenticatedObjectExtents() const {
  return Data->Debug && Data->Debug->hasAuthenticatedObjectExtents();
}

llvm::Expected<std::shared_ptr<const ModuleImage>>
ModuleImage::load(const ModuleInput &Input, const ModuleLimits &Limits,
                  uint64_t AvailableBytes) {
  auto Source = SourceSnapshot::read(
      Input.Image, std::min(AvailableBytes, Limits.ImageBytes));
  if (!Source)
    return Source.takeError();
  auto Image = loadBinary((*Source)->Path);
  if (!Image)
    return Image.takeError();
  if (!Image->InputFileSHA256 ||
      llvm::toHex(llvm::ArrayRef(*Image->InputFileSHA256), true) !=
          (*Source)->Digest)
    return invalid(
        "binary loader did not observe the prepared source identity");
  auto Data = std::make_unique<Storage>();
  Data->ImageSource = std::move(*Source);
  Data->Image = std::make_shared<BinaryImage>(std::move(*Image));
  Data->SymbolLimit = Limits.Symbols;
  auto Result =
      std::shared_ptr<const ModuleImage>(new ModuleImage(std::move(Data)));
  if (Input.Debug)
    return withDebug(*Result, *Input.Debug, Limits,
                     AvailableBytes - Result->sourceBytes());
  return Result;
}

llvm::Expected<std::shared_ptr<const ModuleImage>>
ModuleImage::withDebug(const ModuleImage &Original, const DebugInput &Input,
                       const ModuleLimits &Limits, uint64_t AvailableBytes) {
  if (Original.Data->Debug)
    return invalid("debug information already attached");
  auto Source = SourceSnapshot::read(
      Input.Path, std::min(AvailableBytes, Limits.DebugBytes));
  if (!Source)
    return Source.takeError();
  auto Debug = loadDebug(*Original.Data->ImageSource, Original.image(),
                         **Source, Input.Kind);
  if (!Debug)
    return Debug.takeError();
  auto Data = std::make_unique<Storage>();
  Data->ImageSource = Original.Data->ImageSource;
  Data->Image = Original.Data->Image;
  Data->DebugSource = std::move(*Source);
  Data->Debug = std::move(*Debug);
  Data->Kind = Input.Kind;
  Data->SymbolLimit = Original.Data->SymbolLimit;
  return std::shared_ptr<const ModuleImage>(new ModuleImage(std::move(Data)));
}

llvm::Expected<std::optional<ModuleSymbol>>
ModuleImage::lookupSymbol(llvm::StringRef Name) const {
  if (Name.empty() ||
      Name.size() >
          ::neverd::limits::modules::kModuleImageLookupSymbolNameSizeLimit ||
      Name.contains('\0'))
    return invalid("symbol name is empty or exceeds its limit");
  std::optional<ModuleSymbol> Result;
  uint64_t Visited = 0;
  auto Observe = [&](llvm::StringRef N, uint64_t Address, uint64_t Size,
                     SymbolKind Kind) -> llvm::Error {
    if (++Visited > Data->SymbolLimit)
      return invalid("symbol query exceeds its limit");
    if (N != Name || Address == InvalidVA)
      return llvm::Error::success();
    if (Result && (Result->Address != Address || Result->Kind != Kind))
      return invalid("ambiguous symbol: " + Name);
    if (!Result)
      Result = ModuleSymbol{N.str(), Address, Size, Kind};
    else if (Size && (!Result->Size || Size < Result->Size))
      Result->Size = Size;
    return llvm::Error::success();
  };
  for (const auto &S : image().Symbols)
    if (auto E = Observe(S.Name, S.Addr, S.Size,
                         S.IsFunc ? SymbolKind::Function : SymbolKind::Data))
      return std::move(E);
  for (const auto &S : image().Exports) {
    const bool Code = llvm::any_of(image().Segments, [&](const auto &Segment) {
      return Segment.isExecutable() && Segment.contains(S.Addr);
    });
    if (auto E = Observe(S.Name, S.Addr, 0,
                         Code ? SymbolKind::Function : SymbolKind::Data))
      return std::move(E);
  }
  if (Data->Debug) {
    for (const auto &S : Data->Debug->allFunctions())
      if (auto E = Observe(S.Name, S.Addr, S.Size, SymbolKind::Function))
        return std::move(E);
    for (const auto &S : Data->Debug->allDataObjects())
      if (auto E = Observe(S.Name, S.Addr, S.Size, SymbolKind::Data))
        return std::move(E);
  }
  return Result;
}
} // namespace neverd::modules
