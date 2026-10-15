//===- Probe.cpp - Prebuilt LLVM and host C++ runtime compatibility --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Link and execute the actual packaged LLVM consumers before a product build.
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/Binary.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

int main() {
  llvm::LLVMContext Context;
  llvm::Module Module("runtime-compatibility", Context);
  if (Module.getDataLayout().getLargestLegalIntTypeSizeInBits() != 0 ||
      llvm::StringRef("xxruntime").find_first_not_of('x') != 2)
    return 1;
  auto Invalid = llvm::object::createBinary(
      llvm::MemoryBufferRef("inert invalid binary", "runtime-probe"));
  if (Invalid)
    return 1;
  llvm::consumeError(Invalid.takeError());
  return 0;
}
