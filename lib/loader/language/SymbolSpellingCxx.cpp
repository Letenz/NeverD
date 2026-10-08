//===- SymbolSpellingCxx.cpp - Itanium and Microsoft C++ names ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C++ names read as LLVM's demanglers print them.  Their C identifier stems
/// follow the C backend's own C++ rules (CIdentifier.h), so this file gives
/// none.
///
//===----------------------------------------------------------------------===//

#include "SymbolSpellingDetail.h"

#include "llvm/Demangle/Demangle.h"

using namespace neverd;
using namespace neverd::symbol_spelling;

bool symbol_spelling::claimsItaniumName(llvm::StringRef) { return true; }

std::string symbol_spelling::readableItaniumName(llvm::StringRef Name) {
  return takeDemangled(llvm::itaniumDemangle(Name));
}

std::string symbol_spelling::stemOfItaniumName(llvm::StringRef) { return {}; }

bool symbol_spelling::claimsMicrosoftName(llvm::StringRef) { return true; }

std::string symbol_spelling::readableMicrosoftName(llvm::StringRef Name) {
  // `public:` says nothing a reader of decompiled code can use; the calling
  // convention and the return type do.
  return takeDemangled(llvm::microsoftDemangle(Name, nullptr, nullptr,
                                               llvm::MSDF_NoAccessSpecifier));
}

std::string symbol_spelling::stemOfMicrosoftName(llvm::StringRef) { return {}; }
