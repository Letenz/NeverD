#pragma once
#include "neverd/ir/SourceVariableEdits.h"

#include <string>
namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm
namespace neverd::llvmc {
std::string sourceValueId(const llvm::Instruction &Instruction, unsigned Index);
std::string sourceStorageId(const llvm::Value *Pointer, unsigned Bytes);
SourceVariableEditResult editVariableStorage(llvm::Function &Function,
                                             const SourceVariableEdit &Edit,
                                             unsigned EditIndex);
void captureSourceValueIds(llvm::Function &Function);
} // namespace neverd::llvmc
