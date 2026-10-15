//===- COFFSourceCallSymbols.h - Exact source call symbols ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep preserved code entries and imported function slots in separate domains.

#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFSOURCECALLSYMBOLS_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFSOURCECALLSYMBOLS_H

#include "neverd/Common.h"

#include "llvm/Support/Error.h"

#include <map>

namespace llvm {
class Module;
}
namespace neverd {
struct BinaryImage;
struct COFFSourceCallSymbols {
  std::map<std::string, va_t> Code;
  std::map<std::string, va_t> Imports;
};
llvm::Expected<COFFSourceCallSymbols>
getCOFFSourceCallSymbols(const llvm::Module &Module, const BinaryImage &Image);
} // namespace neverd

#endif
