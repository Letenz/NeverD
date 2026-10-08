//===- SymbolSpellingD.cpp - D symbol names -------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// D names, read by LLVM's D demangler: `d_eh_probe.cleanupCount`.  A C
/// identifier stem joins the words of the qualified name.
///
//===----------------------------------------------------------------------===//

#include "SymbolSpellingDetail.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"

#include <vector>

using namespace neverd;
using namespace neverd::symbol_spelling;

bool symbol_spelling::claimsDName(llvm::StringRef Name) {
  // `_D` and the length of the first identifier of the qualified name.
  return Name.size() > 2 && llvm::isDigit(Name[2]);
}

std::string symbol_spelling::readableDName(llvm::StringRef Name) {
  return takeDemangled(llvm::dlangDemangle(Name));
}

std::string symbol_spelling::stemOfDName(llvm::StringRef Name) {
  std::string Readable = readableDName(Name);
  // The qualified name ends where a function's parameter list starts.
  llvm::StringRef Qualified = llvm::StringRef(Readable).take_until(
      [](char C) { return C == '(' || C == ' '; });
  llvm::SmallVector<llvm::StringRef, 8> Parts;
  Qualified.split(Parts, '.', -1, /*KeepEmpty=*/false);
  std::vector<std::string> Words;
  for (llvm::StringRef Part : Parts)
    Words.push_back(Part.str());
  return joinStemWords(Words);
}
