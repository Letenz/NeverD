//===- HighCSourceNames.cpp - The names the emitted C spells --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Records which symbol each function and object identifier of the emitted C
/// stands for (CSourceMap::Names), so that a reader of the text can name it
/// as its source language does: `rust_eh_probe_main` is
/// `_RNvCs8vGhbR5OvgK_13rust_eh_probe4main`, `rust_eh_probe::main` in Rust.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/backend/c/CSourceMap.h"

#include <map>

using namespace neverd;

void HighCWriter::recordSourceNames(const std::vector<HighFunc> &Funcs) {
  if (!Opts.SourceMap && !Opts.SourceNames)
    return;
  std::map<std::string, CSourceName> ByIdentifier;
  auto Add = [&](CSourceName::Kind Kind, const std::string &Identifier,
                 llvm::StringRef Symbol, std::optional<va_t> Address) {
    if (Identifier.empty())
      return;
    auto [It, Inserted] = ByIdentifier.try_emplace(Identifier);
    CSourceName &Name = It->second;
    if (Inserted) {
      Name.TheKind = Kind;
      Name.Identifier = Identifier;
      Name.Symbol = Symbol.str();
      Name.Address = Address;
      return;
    }
    // One identifier for several symbols, such as an MSVC stem the MSVC rules
    // share between overloads, names none of them.
    if (Name.Symbol != Symbol)
      Name.Symbol.clear();
    if (Address && Name.Address != Address)
      Name.Address.reset();
  };
  for (const HighFunc &Func : Funcs)
    if (auto It = FunctionIdentifiers.find(&Func);
        It != FunctionIdentifiers.end())
      Add(CSourceName::Kind::Function, It->second, Func.Name, Func.Entry);
  for (const auto &[Source, Identifier] : ExternalSourceIdentifiers)
    Add(CSourceName::Kind::Function, Identifier, Source, std::nullopt);
  for (const auto &[Source, Identifier] : ExternalFunctionIdentifiers)
    if (!ByIdentifier.count(Identifier))
      Add(CSourceName::Kind::Function, Identifier, Source, std::nullopt);
  for (const auto &[Identifier, Source] : ReferencedFunctionSymbols)
    if (!ByIdentifier.count(Identifier))
      Add(CSourceName::Kind::Function, Identifier, Source, std::nullopt);
  for (const auto &[Address, Object] : ImageObjects)
    Add(CSourceName::Kind::Object, Object.Name, Object.Symbol, Address);
  std::vector<CSourceName> Names;
  Names.reserve(ByIdentifier.size());
  for (auto &[Identifier, Name] : ByIdentifier)
    Names.push_back(std::move(Name));
  if (Opts.SourceNames)
    *Opts.SourceNames = Names;
  if (Opts.SourceMap)
    Opts.SourceMap->Names = std::move(Names);
}
