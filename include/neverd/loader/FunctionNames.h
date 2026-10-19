//===- FunctionNames.h - Function names of many addresses ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_FUNCTIONNAMES_H
#define NEVERD_LOADER_FUNCTIONNAMES_H

#include "neverd/loader/BinaryImage.h"

#include <map>
#include <set>
#include <string>

namespace neverd {

/// BinaryImage::getFunctionNameAt() for each of \p Addrs, from one pass over
/// the exports and symbols rather than one per address: the first export
/// there when it has a name, else the symbol of the highest functionNameRank
/// (the first of equal rank), as the function is listed.
inline std::map<va_t, std::string>
functionNamesAt(const BinaryImage &Img, const std::set<va_t> &Addrs) {
  std::map<va_t, const Export *> FirstExports;
  for (const Export &Exp : Img.Exports)
    if (Addrs.count(Exp.Addr))
      FirstExports.try_emplace(Exp.Addr, &Exp);
  std::map<va_t, const Symbol *> BestSymbols;
  for (const Symbol &Sym : Img.Symbols) {
    if (Sym.Name.empty() || Sym.IsSection || !Addrs.count(Sym.Addr))
      continue;
    auto [It, Inserted] = BestSymbols.try_emplace(Sym.Addr, &Sym);
    if (!Inserted && BinaryImage::functionNameRank(Sym) >
                         BinaryImage::functionNameRank(*It->second))
      It->second = &Sym;
  }
  std::map<va_t, std::string> Names;
  for (va_t Addr : Addrs) {
    const auto Exp = FirstExports.find(Addr);
    const auto Sym = BestSymbols.find(Addr);
    if (Exp != FirstExports.end() && !Exp->second->Name.empty())
      Names.emplace(Addr, Exp->second->Name);
    else if (Sym != BestSymbols.end())
      Names.emplace(Addr, Img.functionListName(Addr, Sym->second->Name));
    else
      Names.emplace(Addr, (kAutoFuncPrefix + llvm::utohexstr(Addr)).str());
  }
  return Names;
}

} // namespace neverd

#endif // NEVERD_LOADER_FUNCTIONNAMES_H
