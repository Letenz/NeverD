//===- ModuleSet.cpp - Incremental module preparation and inspection -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Incremental module preparation and inspection.
///

#include "neverd/modules/ModuleSet.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace neverd::modules {
namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "module catalogue: " + Message);
}
} // namespace

llvm::Expected<std::unique_ptr<ModuleSet>>
ModuleSet::create(ModuleOptions Input) {
  auto Normalized = normalizeModuleOptions(std::move(Input));
  if (!Normalized)
    return Normalized.takeError();
  auto Initial = std::move(Normalized->Modules);
  Normalized->Modules.clear();
  auto Result =
      std::unique_ptr<ModuleSet>(new ModuleSet(std::move(*Normalized)));
  for (auto &M : Initial) {
    auto ID = M.ID;
    Result->Entries.emplace(std::move(ID), Entry{std::move(M), {}});
  }
  return Result;
}

llvm::Expected<std::unique_ptr<ModuleSet>>
ModuleSet::fromJSON(llvm::StringRef JSON,
                    const std::filesystem::path &BaseDirectory) {
  auto Options = moduleOptionsFromJSON(JSON, BaseDirectory);
  if (!Options)
    return Options.takeError();
  return create(std::move(*Options));
}

llvm::Error ModuleSet::add(ModuleInput Input) {
  if (Entries.size() >= Options.Limits.Modules)
    return invalid("module count exceeds its limit");
  ModuleOptions One = Options;
  One.Modules.push_back(std::move(Input));
  auto Checked = normalizeModuleOptions(std::move(One));
  if (!Checked)
    return Checked.takeError();
  auto M = std::move(Checked->Modules.front());
  if (Entries.contains(M.ID))
    return invalid("duplicate module id: " + M.ID);
  auto ID = M.ID;
  Entries.emplace(std::move(ID), Entry{std::move(M), {}});
  ++Revision;
  return llvm::Error::success();
}

llvm::Expected<std::shared_ptr<const ModuleImage>>
ModuleSet::prepare(llvm::StringRef ID) {
  auto I = Entries.find(ID.str());
  if (I == Entries.end())
    return invalid("unknown module id: " + ID);
  if (I->second.Image)
    return I->second.Image;
  auto Image = ModuleImage::load(I->second.Input, Options.Limits,
                                 Options.Limits.TotalSourceBytes - SourceBytes);
  if (!Image)
    return Image.takeError();
  SourceBytes += (*Image)->sourceBytes();
  I->second.Image = *Image;
  ++Revision;
  return *Image;
}

llvm::Error ModuleSet::reviseUnprepared(llvm::StringRef ID,
                                        const std::filesystem::path &Image,
                                        std::optional<DebugInput> Debug) {
  auto I = Entries.find(ID.str());
  if (I == Entries.end())
    return invalid("unknown module id: " + ID);
  if (I->second.Image)
    return invalid("prepared module input cannot be replaced");
  if (Revision == UINT64_MAX)
    return invalid("catalogue revision exhausted");
  auto Input = I->second.Input;
  Input.Image = Image;
  Input.Debug = std::move(Debug);
  ModuleOptions One = Options;
  One.Modules.push_back(std::move(Input));
  auto Checked = normalizeModuleOptions(std::move(One));
  if (!Checked)
    return Checked.takeError();
  I->second.Input = std::move(Checked->Modules.front());
  ++Revision;
  return llvm::Error::success();
}

llvm::Error ModuleSet::prepareEager() {
  uint64_t Total = SourceBytes;
  std::vector<std::pair<Entry *, std::shared_ptr<const ModuleImage>>> Prepared;
  for (auto &[ID, Entry] : Entries) {
    (void)ID;
    if (Entry.Image ||
        Entry.Input.Load.value_or(Options.DefaultLoad) != LoadPolicy::Eager)
      continue;
    auto Image = ModuleImage::load(Entry.Input, Options.Limits,
                                   Options.Limits.TotalSourceBytes - Total);
    if (!Image)
      return Image.takeError();
    Total += (*Image)->sourceBytes();
    Prepared.emplace_back(&Entry, std::move(*Image));
  }
  if (Prepared.empty())
    return llvm::Error::success();
  for (auto &[Entry, Image] : Prepared)
    Entry->Image = std::move(Image);
  SourceBytes = Total;
  ++Revision;
  return llvm::Error::success();
}

llvm::Expected<std::shared_ptr<const ModuleImage>>
ModuleSet::attachDebug(llvm::StringRef ID, DebugInput Debug) {
  auto I = Entries.find(ID.str());
  if (I == Entries.end())
    return invalid("unknown module id: " + ID);
  if (I->second.Input.Debug)
    return invalid(
        "debug information is already declared; replacement is unsupported");
  ModuleOptions One = Options;
  auto Input = I->second.Input;
  Input.Debug = std::move(Debug);
  One.Modules.push_back(std::move(Input));
  auto Checked = normalizeModuleOptions(std::move(One));
  if (!Checked)
    return Checked.takeError();
  auto M = std::move(Checked->Modules.front());
  const uint64_t Available = Options.Limits.TotalSourceBytes - SourceBytes;
  auto Image = I->second.Image
                   ? ModuleImage::withDebug(*I->second.Image, *M.Debug,
                                            Options.Limits, Available)
                   : ModuleImage::load(M, Options.Limits, Available);
  if (!Image)
    return Image.takeError();
  const uint64_t OldBytes =
      I->second.Image ? I->second.Image->sourceBytes() : 0;
  SourceBytes += (*Image)->sourceBytes() - OldBytes;
  I->second.Input = std::move(M);
  I->second.Image = *Image;
  ++Revision;
  return *Image;
}

std::shared_ptr<const ModuleImage>
ModuleSet::prepared(llvm::StringRef ID) const {
  auto I = Entries.find(ID.str());
  return I == Entries.end() ? nullptr : I->second.Image;
}

std::vector<ModuleInfo> ModuleSet::inspect() const {
  std::vector<ModuleInfo> Result;
  for (const auto &[ID, E] : Entries) {
    (void)ID;
    ModuleInfo Info;
    Info.Input = E.Input;
    Info.Load = E.Input.Load.value_or(Options.DefaultLoad);
    Info.Prepared = bool(E.Image);
    if (E.Image) {
      Info.ImageSHA256 = E.Image->imageDigest();
      Info.DebugSHA256 = E.Image->debugDigest();
      Info.SourceBytes = E.Image->sourceBytes();
    }
    Result.push_back(std::move(Info));
  }
  return Result;
}

llvm::Expected<std::optional<ModuleSymbol>>
ModuleSet::lookupSymbol(llvm::StringRef ID, llvm::StringRef Name) {
  auto Image = prepare(ID);
  if (!Image)
    return Image.takeError();
  return (*Image)->lookupSymbol(Name);
}

std::string moduleSetJSON(const ModuleSet &Modules) {
  llvm::json::Array Rows;
  for (const auto &Info : Modules.inspect()) {
    llvm::json::Object Row{{"id", Info.Input.ID},
                           {"image", pathToUTF8(Info.Input.Image)},
                           {"load", loadPolicyName(Info.Load)},
                           {"prepared", Info.Prepared},
                           {"source_bytes", Info.SourceBytes}};
    if (Info.Input.Debug)
      Row["debug"] =
          llvm::json::Object{{"kind", debugKindName(Info.Input.Debug->Kind)},
                             {"path", pathToUTF8(Info.Input.Debug->Path)}};
    if (auto Image = Modules.prepared(Info.Input.ID)) {
      Row["image_sha256"] = Info.ImageSHA256;
      Row["format"] = Image->image().getFormatName();
      Row["architecture"] = getArchName(Image->image().Arch);
      Row["debug_loaded"] = bool(Image->debug());
      Row["debug_identity_verified"] =
          Image->debug() && Image->debugKind() == DebugKind::PDB;
      if (const auto &Identity = Image->image().DynInfo.CodeViewPDBIdentity) {
        llvm::json::Object CodeView{
            {"kind", Identity->Kind == PDBIdentityKind::RSDS ? "rsds" : "nb10"},
            {"age", Identity->Age}};
        if (Identity->Kind == PDBIdentityKind::RSDS)
          CodeView["guid_bytes"] =
              llvm::toHex(llvm::ArrayRef(Identity->Guid), true);
        else
          CodeView["signature"] = Identity->Signature;
        Row["image_codeview_identity"] = std::move(CodeView);
      }
      Row["preferred_base"] = "0x" + llvm::utohexstr(Image->image().Base);
      Row["debug_sha256"] = Info.DebugSHA256;
      Row["authenticated_function_signatures"] =
          Image->hasAuthenticatedFunctionSignatures();
      Row["authenticated_object_extents"] =
          Image->hasAuthenticatedObjectExtents();
    }
    Rows.push_back(std::move(Row));
  }
  return llvm::formatv("{0:2}", llvm::json::Value(llvm::json::Object{
                                    {"revision", Modules.revision()},
                                    {"retained_source_bytes",
                                     Modules.retainedSourceBytes()},
                                    {"modules", std::move(Rows)}}))
      .str();
}
} // namespace neverd::modules
