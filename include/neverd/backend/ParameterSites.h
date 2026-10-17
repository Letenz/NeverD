//===- ParameterSites.h - Where lifted parameters arrive --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the IR attachment that records where each argument of a lifted
/// function arrives: the register the machine passes it in, or the MedIR
/// parameter index that numbers its stack slot.  A writer binds a debug
/// signature's parameters to the arguments by these places.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_PARAMETERSITES_H
#define NEVERD_BACKEND_PARAMETERSITES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::parameter_sites {

inline constexpr llvm::StringLiteral
    FunctionAttachment("neverd.parameter-sites");
inline constexpr uint32_t SchemaVersion = 1;

/// Where one argument arrives.
struct Site {
  /// The register's offset, or ~0 for a stack slot.
  uint64_t RegOff = ~uint64_t(0);
  /// The MedIR parameter index.
  int64_t MedIndex = -1;
};

/// Records \p Sites, one per argument of \p Function in order.
inline void setParameterSites(llvm::Function &Function,
                              llvm::ArrayRef<Site> Sites) {
  llvm::LLVMContext &Context = Function.getContext();
  auto UInt = [&](uint64_t Value, unsigned Width) -> llvm::Metadata * {
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::IntegerType::get(Context, Width), Value));
  };
  std::vector<llvm::Metadata *> Operands = {UInt(SchemaVersion, 32)};
  for (const Site &S : Sites) {
    Operands.push_back(UInt(S.RegOff, 64));
    Operands.push_back(UInt(static_cast<uint64_t>(S.MedIndex), 64));
  }
  Function.setMetadata(FunctionAttachment,
                       llvm::MDNode::get(Context, Operands));
}

/// The sites \p Function records, one per argument; none when it records
/// none, or not one for every argument.
inline std::optional<std::vector<Site>>
getParameterSites(const llvm::Function &Function) {
  const llvm::MDNode *Node = Function.getMetadata(FunctionAttachment);
  if (!Node || Node->getNumOperands() != 1 + 2 * Function.arg_size())
    return std::nullopt;
  auto Read = [&](unsigned Index, unsigned Width) -> std::optional<uint64_t> {
    const auto *Constant = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
        Node->getOperand(Index).get());
    const auto *Integer =
        Constant ? llvm::dyn_cast<llvm::ConstantInt>(Constant->getValue())
                 : nullptr;
    if (!Integer || Integer->getBitWidth() != Width)
      return std::nullopt;
    return Integer->getZExtValue();
  };
  if (Read(0, 32) != SchemaVersion)
    return std::nullopt;
  std::vector<Site> Sites;
  for (unsigned I = 1; I + 1 < Node->getNumOperands(); I += 2) {
    const auto RegOff = Read(I, 64);
    const auto Index = Read(I + 1, 64);
    if (!RegOff || !Index)
      return std::nullopt;
    Sites.push_back({*RegOff, static_cast<int64_t>(*Index)});
  }
  return Sites;
}

} // namespace neverd::parameter_sites

#endif // NEVERD_BACKEND_PARAMETERSITES_H
