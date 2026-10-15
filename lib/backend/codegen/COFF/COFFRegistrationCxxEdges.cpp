//===- COFFRegistrationCxxEdges.cpp - Check PE32 scalar copy edges -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCopyEdges.h"
#include "COFFRegistrationCxxIRProof.h"

#include "neverd/Limits.h"

#include "llvm/IR/CFG.h"

namespace neverd::coff_registration {
llvm::Error bindCxxCopyEdges(CxxIRControlProof &Proof) {
  size_t Work = 0;
  for (const auto &Source : Proof.Source.Blocks) {
    const auto Segment = Proof.Segments.find(Source.Id);
    if (Segment == Proof.Segments.end())
      continue;
    const auto *From = Segment->second.Exit->getParent();
    const auto *Term = From->getTerminator();
    if (!llvm::isa<llvm::CondBrInst, llvm::UncondBrInst, llvm::SwitchInst>(
            Term))
      continue;
    std::set<const llvm::BasicBlock *> Expected;
    for (int Successor : Source.Succs) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("C++ copy-edge proof exceeded its work budget");
      if (Proof.Segments.count(Successor))
        Expected.insert(Proof.Segments.at(Successor).Enter->getParent());
    }
    for (const auto *Block : llvm::successors(From)) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("C++ copy-edge proof exceeded its work budget");
      if (Expected.count(Block))
        continue;
      const auto *Destination =
          registrationCopyDestination(*From, *Block, Expected, Work);
      if (!Destination)
        return rejectIR("C++ source edge gained unproved copy instructions");
      const auto [Found, New] = Proof.CopyEdges.emplace(Block, Destination);
      if (!New && Found->second != Destination)
        return rejectIR("C++ copy block has conflicting source edges");
      const auto Owner = Proof.SourceCatchOwners.find(Source.Id);
      if (Owner != Proof.SourceCatchOwners.end())
        Proof.Catches.at(Owner->second).Blocks.insert(Block);
    }
  }
  return llvm::Error::success();
}
} // namespace neverd::coff_registration
