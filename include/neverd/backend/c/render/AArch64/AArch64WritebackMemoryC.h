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

#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.h"
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
  if (Instruction.isVector() && Instruction.isLoad())
    throw std::invalid_argument(kWritebackMalformed);
  if (Instruction.isLoad())
    return llvm::formatv(kWritebackLoadCExpression, ValueName, BaseName, Base,
                         Instruction.scratchWord())
        .str();
  const auto BaseInput =
      aarch64PAuthTemporary(kWritebackCBase, {Base, Data, ValueName, BaseName});
  const auto DataInput = aarch64PAuthTemporary(
      kWritebackCData, {Base, Data, ValueName, BaseName, BaseInput});
  if (Instruction.isVector())
    return llvm::formatv(
               kWritebackVectorStoreCExpression, ValueName, BaseName, Base,
               Instruction.scratchWord(), BaseInput, DataInput, Data,
               llvm::formatv(kAddressVectorFromBitsC, DataInput).str())
        .str();
  return llvm::formatv(kWritebackStoreCExpression, ValueName, BaseName, Base,
                       Instruction.scratchWord(), BaseInput, DataInput, Data)
      .str();
}

inline std::string aarch64VectorWritebackMemoryCStatement(
    const AArch64WritebackMemoryInstruction &Instruction, llvm::StringRef Base,
    llvm::StringRef ValueOutput, llvm::StringRef BaseOutput,
    bool VectorValue = false) {
  if (!aarch64WritebackMemoryInstruction(Instruction.Word) ||
      !Instruction.isVector() || !Instruction.isLoad())
    throw std::invalid_argument(kWritebackMalformed);
  if (Instruction.stackBase())
    throw std::invalid_argument(kWritebackSPUnsupported);
  const auto Value =
      aarch64PAuthTemporary(kWritebackCValue, {Base, ValueOutput, BaseOutput});
  const auto Updated = aarch64PAuthTemporary(
      kWritebackCBase, {Base, ValueOutput, BaseOutput, Value});
  const auto Input = aarch64PAuthTemporary(
      kWritebackCBase, {Base, ValueOutput, BaseOutput, Value, Updated});
  std::string Assignments;
  if (!ValueOutput.empty())
    Assignments +=
        llvm::formatv(kWritebackCAssignment, ValueOutput,
                      VectorValue
                          ? Value
                          : llvm::formatv(kAddressVectorToBitsC, Value).str())
            .str();
  if (!BaseOutput.empty())
    Assignments +=
        llvm::formatv(kWritebackCAssignment, BaseOutput, Updated).str();
  return llvm::formatv(kWritebackVectorLoadCStatement, Value, Updated, Base,
                       Instruction.scratchWord(), Input, Assignments)
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64WRITEBACKMEMORYC_H
