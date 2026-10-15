//===- StateSourceBank.h - Shared state-bank address and effect facts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Internal bank proofs shared by source reconstruction and optimization.
#ifndef NEVERD_PIPELINE_STATESOURCEBANK_H
#define NEVERD_PIPELINE_STATESOURCEBANK_H
#include "llvm/ADT/BitVector.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>
namespace llvm {
class CallInst;
class DataLayout;
class Function;
class Module;
class Value;
} // namespace llvm
namespace neverd {
struct StateSourceRecovery;
namespace state_source {
struct Offsets {
  std::vector<int64_t> Bank;
  bool Guest = false;
};
std::optional<Offsets> bankOffsets(llvm::Value *, llvm::Value *,
                                   const llvm::DataLayout &);
std::optional<int64_t> displacement(llvm::Value *, llvm::Value *,
                                    const llvm::DataLayout &);
llvm::Error validateBank(llvm::Function &, unsigned);
using CallWrites = std::map<const llvm::CallInst *, llvm::BitVector>;
void refineBankAccesses(llvm::Module &, StateSourceRecovery &,
                        const CallWrites &);
} // namespace state_source
} // namespace neverd
#endif
