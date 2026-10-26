//===- AArch64WritebackMemoryC.h - Writeback C projection ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared C projection retains access and writeback in one native instruction.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64WRITEBACKMEMORYC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64WRITEBACKMEMORYC_H

#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/ir/high/AArch64WritebackMemory.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_WRITEBACK_C_TEXT(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64WritebackMemoryC.def"

inline std::string
aarch64WritebackMemoryC(const AArch64WritebackMemoryInstruction &Instruction,
                        llvm::StringRef Base, llvm::StringRef Data = {}) {
  if (!aarch64WritebackMemoryInstruction(Instruction.Word) ||
      Instruction.isLoad() != Data.empty())
    throw std::invalid_argument(kWritebackMalformed);
  if (Instruction.stackBase())
    throw std::invalid_argument(kWritebackSPUnsupported);
  const auto ValueName = aarch64PAuthTemporary(kWritebackCValue, {Base, Data});
  const auto BaseName =
      aarch64PAuthTemporary(kWritebackCBase, {Base, Data, ValueName});
  if (Instruction.isLoad())
    return llvm::formatv(kWritebackLoadCExpression, ValueName, BaseName, Base,
                         Instruction.scratchWord())
        .str();
  const auto BaseInput =
      aarch64PAuthTemporary(kWritebackCBase, {Base, Data, ValueName, BaseName});
  const auto DataInput = aarch64PAuthTemporary(
      kWritebackCData, {Base, Data, ValueName, BaseName, BaseInput});
  return llvm::formatv(kWritebackStoreCExpression, ValueName, BaseName, Base,
                       Instruction.scratchWord(), BaseInput, DataInput, Data)
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64WRITEBACKMEMORYC_H
