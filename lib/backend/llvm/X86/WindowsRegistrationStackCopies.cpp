//===- WindowsRegistrationStackCopies.cpp - Private callback SSA copies ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowsRegistrationFramePrivate.h"

#include "neverd/Limits.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"

namespace neverd::x86_registration {
llvm::Error validateRegistrationStackCopies(
    llvm::BasicBlock &Entry, const std::set<llvm::BasicBlock *> &Body,
    const std::map<llvm::AllocaInst *, RegistrationStackCopy> &Copies,
    const std::map<llvm::Value *, int64_t> &Offsets, size_t &Work,
    llvm::ArrayRef<X86RegistrationCatchStackResume> Resumes) {
  auto Charge = [&] {
    return Work < limits::kMaxRegistrationEHStateWork && ++Work;
  };
  for (const auto &[Slot, Copy] : Copies) {
    for (auto *Definition : Copy.Definitions) {
      const auto Source = Offsets.find(Definition->getValueOperand());
      if (!Charge() || Source == Offsets.end() || Source->second != Copy.Offset)
        return reject("private callback stack copy has an unproved source");
    }
    for (auto *Load : Copy.Loads) {
      // Runtime callbacks are disconnected before outlining. Require a
      // checked definition on every ordinary predecessor, including copies
      // inserted on PHI edges. Every definition above has the same offset.
      using Point = std::pair<llvm::BasicBlock *, bool>;
      std::vector<Point> Pending{{Load->getParent(), false}};
      std::set<Point> Seen;
      bool Found = false;
      while (!Pending.empty()) {
        if (!Charge())
          return reject("private callback stack copy exceeds the work budget");
        const auto [Block, Incoming] = Pending.back();
        Pending.pop_back();
        if (!Seen.emplace(Block, Incoming).second)
          continue;
        bool Defined = false;
        for (auto *Definition : Copy.Definitions) {
          if (!Charge())
            return reject(
                "private callback stack copy exceeds the work budget");
          Defined |=
              !Incoming && Definition->getParent() == Block &&
              (Block != Load->getParent() || Definition->comesBefore(Load));
        }
        if (Defined) {
          Found = true;
          continue;
        }
        if (Block == &Entry || !Body.count(Block))
          return reject(
              "private callback stack copy is not initialized on every path");
        bool Resumed = false;
        for (const auto &Resume : Resumes) {
          if (!Charge())
            return reject(
                "private callback stack copy exceeds the work budget");
          if (Block != Resume.Seed->getParent())
            continue;
          Resumed = true;
          // A proved nested continuation preserves this invocation's private
          // cells. It observes each dispatch source's incoming state, so a
          // write later in that source block cannot initialize the resume.
          for (auto *Source : Resume.Sources) {
            if (!Charge())
              return reject(
                  "private callback stack copy exceeds the work budget");
            Pending.emplace_back(Source, true);
          }
        }
        if (!Resumed && llvm::pred_empty(Block))
          return reject(
              "private callback stack copy is not initialized on every path");
        for (auto *Pred : llvm::predecessors(Block)) {
          if (!Charge())
            return reject(
                "private callback stack copy exceeds the work budget");
          Pending.emplace_back(Pred, false);
        }
      }
      if (!Found)
        return reject("private callback stack copy has no reaching definition");
    }
  }
  return llvm::Error::success();
}
} // namespace neverd::x86_registration
