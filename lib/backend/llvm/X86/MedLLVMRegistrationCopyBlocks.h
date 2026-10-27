//===- MedLLVMRegistrationCopyBlocks.h - PE32 copy owners -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_MEDLLVMREGISTRATIONCOPYBLOCKS_H
#define NEVERD_MEDLLVMREGISTRATIONCOPYBLOCKS_H

#include <map>
#include <optional>
#include <utility>

namespace llvm {
class BasicBlock;
} // namespace llvm

namespace neverd {
bool hasCheckedRegistrationPHICopyEdges(
    const std::map<int, llvm::BasicBlock *> &OriginalBlocks,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &CopyEdges);

/// A compiler-created copy block inherits ownership from the exact edge split
/// by this emission. Installation independently checks its code and storage.
template <typename Identity>
std::optional<std::map<llvm::BasicBlock *, Identity>> registrationPHICopyOwners(
    const std::map<int, Identity> &Owners,
    const std::map<int, llvm::BasicBlock *> &OriginalBlocks,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &CopyEdges,
    const std::map<int, int> &ParentExits = {}) {
  if (!hasCheckedRegistrationPHICopyEdges(OriginalBlocks, CopyEdges))
    return std::nullopt;
  std::map<llvm::BasicBlock *, Identity> Result;
  for (const auto &[Block, Edge] : CopyEdges) {
    const auto Parent = Owners.find(Edge.first);
    const auto Target = Owners.find(Edge.second);
    const auto Exit = ParentExits.find(Edge.first);
    if (Parent != Owners.end() && Target == Owners.end() &&
        Exit != ParentExits.end() && Exit->second == Edge.second) {
      Result.emplace(Block, Parent->second);
      continue;
    }
    if ((Parent == Owners.end()) != (Target == Owners.end()) ||
        (Parent != Owners.end() && Parent->second != Target->second))
      return std::nullopt;
    if (Parent != Owners.end())
      Result.emplace(Block, Parent->second);
  }
  return Result;
}
} // namespace neverd

#endif // NEVERD_MEDLLVMREGISTRATIONCOPYBLOCKS_H
