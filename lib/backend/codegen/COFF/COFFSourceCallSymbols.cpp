//===- COFFSourceCallSymbols.cpp - Decorated source call identities -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve decorated declarations to their exact source code or IAT identity.

#include "COFFSourceCallSymbols.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"

namespace neverd {

llvm::Expected<COFFSourceCallSymbols>
getCOFFSourceCallSymbols(const llvm::Module &Module, const BinaryImage &Image) {
  COFFSourceCallSymbols Result;
  for (const auto &Function : Module) {
    if (!Function.isDeclaration())
      continue;
    auto Address = rewrite_source::getOriginalVA(Function);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      continue;
    llvm::SmallString<64> ObjectName;
    llvm::Mangler Mangler;
    Mangler.getNameWithPrefix(ObjectName, &Function, false);
    if (Function.hasDLLImportStorageClass()) {
      const auto *Import = Image.findImportAt(**Address);
      if (!Import || Import->IsDelayImport ||
          Function.getName() != Import->Name ||
          !isFileBackedPEImportSlot(Image, **Address))
        return llvm::createStringError(
            "COFF source import lost its exact IAT identity");
      const std::string Name = "__imp_" + ObjectName.str().str();
      if (!Result.Imports.emplace(Name, **Address).second ||
          Result.Code.count(Name))
        return llvm::createStringError(
            "COFF source import symbol is ambiguous");
      continue;
    }
    if (!Image.isCodeAddress(**Address) || !Image.readVA(**Address, 1))
      continue;
    if (!Result.Code.emplace(ObjectName.str().str(), **Address).second ||
        Result.Imports.count(ObjectName.str().str()))
      return llvm::createStringError(
          "COFF preserved source symbol is ambiguous");
  }
  return Result;
}

} // namespace neverd
