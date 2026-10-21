#include "LLVMCVariableEdits.h"

#include "neverd/backend/Limits.h"
#include "neverd/ir/SourceVariableFlow.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <map>
#include <set>
using namespace neverd;
namespace {
constexpr char Identity[] = "neverd.source.variable";
std::optional<std::pair<const llvm::AllocaInst *, int64_t>>
storage(const llvm::Value *Pointer) {
  const auto *Instruction = llvm::dyn_cast<llvm::Instruction>(Pointer);
  if (!Instruction || !Instruction->getModule())
    return {};
  int64_t Offset = 0;
  const auto *Base = llvm::GetPointerBaseWithConstantOffset(
      Pointer, Offset, Instruction->getModule()->getDataLayout());
  const auto *Allocation = llvm::dyn_cast<llvm::AllocaInst>(Base);
  if (!Allocation || Offset < 0 || Allocation->getAddressSpace() != 0 ||
      !Allocation->isStaticAlloca())
    return {};
  return std::pair{Allocation, Offset};
}
bool scalar(llvm::Type *Type) {
  return Type->isIntegerTy() || Type->isPointerTy() ||
         Type->isFloatingPointTy();
}
struct Slot {
  const llvm::AllocaInst *Allocation;
  int64_t Offset;
  llvm::Type *Type;
  std::vector<std::pair<llvm::Instruction *, size_t>> Accesses;
};
struct Projection {
  llvm::Function &Function;
  SourceVariableFlow Flow;
  std::vector<Slot> Slots;
  std::map<std::string, unsigned> Ids;
  std::map<const llvm::Instruction *, size_t> Nodes;
  std::map<std::pair<const llvm::AllocaInst *, int64_t>, unsigned> Locations;
  explicit Projection(llvm::Function &Function) : Function(Function) {
    for (auto &Block : Function)
      for (auto &I : Block) {
        if (Nodes.size() == ::neverd::limits::backend::
                                kLLVMCVariableEditsProjectionNodesSizeLimit) {
          Flow.Refusal = "The function exceeds the variable edit budget";
          return;
        }
        if (I.isEHPad() ||
            llvm::isa<llvm::InvokeInst, llvm::CallBrInst, llvm::IndirectBrInst>(
                &I)) {
          Flow.Refusal = "Variable storage edits require complete ordinary "
                         "control flow without exception entries";
          return;
        }
        Nodes[&I] = Flow.Nodes.size();
        Flow.Nodes.emplace_back();
      }
    if (Nodes.size() > ::neverd::limits::backend::
                           kLLVMCVariableEditsProjectionNodesSizeLimit ||
        Function.empty()) {
      Flow.Refusal = "The function exceeds the variable edit budget";
      return;
    }
    Flow.Entry = Nodes.at(&Function.getEntryBlock().front());
    for (auto &Block : Function)
      for (auto &I : Block) {
        const auto N = Nodes.at(&I);
        if (I.isTerminator()) {
          for (const auto *Successor : llvm::successors(&Block))
            Flow.Nodes[N].Successors.push_back(Nodes.at(&Successor->front()));
        } else
          Flow.Nodes[N].Successors.push_back(Nodes.at(I.getNextNode()));
        llvm::Value *Pointer = nullptr;
        llvm::Type *Type = nullptr;
        bool Simple = false, Write = false;
        if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I)) {
          Pointer = L->getPointerOperand();
          Type = L->getType();
          Simple = L->isSimple();
        }
        if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          Pointer = S->getPointerOperand();
          Type = S->getValueOperand()->getType();
          Simple = S->isSimple();
          Write = true;
        }
        if (!Pointer)
          continue;
        auto Location = storage(Pointer);
        if (!Location)
          continue;
        const auto &[Allocation, Offset] = *Location;
        const auto &Layout = Function.getParent()->getDataLayout();
        if (!Type->isSized() || !scalar(Type))
          continue;
        const auto Size = Layout.getTypeStoreSize(Type);
        if (Size.isScalable() || !Size.getFixedValue() ||
            Size.getFixedValue() > 16)
          continue;
        const auto Bytes = Size.getFixedValue();
        unsigned V;
        const auto It = Locations.find(*Location);
        if (It != Locations.end()) {
          V = It->second;
          if (Slots[V].Type != Type)
            refuse(V, "The storage has mixed scalar views or partial accesses");
        } else {
          V = Slots.size();
          Locations[*Location] = V;
          Slots.push_back({Allocation, Offset, Type, {}});
          const auto Id = llvmc::sourceStorageId(Pointer, Bytes);
          Ids[Id] = V;
          std::string Key;
          llvm::raw_string_ostream OS(Key);
          Type->print(OS);
          Flow.Variables.push_back({Id, Key, static_cast<unsigned>(Bytes)});
          const auto Extent = Allocation->getAllocationSize(Layout);
          if (!Extent || Extent->isScalable() ||
              uint64_t(Offset) > Extent->getFixedValue() ||
              Bytes > Extent->getFixedValue() - Offset)
            refuse(V, "The access is outside the local allocation");
        }
        if (!Simple)
          refuse(V, "Volatile and atomic storage cannot be split or mapped");
        Slots[V].Accesses.emplace_back(&I, N);
        (Write ? Flow.Nodes[N].Writes : Flow.Nodes[N].Reads).push_back(V);
      }
    size_t ProofWork = 0;
    for (unsigned V = 0; V < Slots.size(); ++V) {
      const auto &S = Slots[V];
      for (unsigned U = 0; U < V; ++U) {
        if (++ProofWork > ::neverd::limits::backend::
                              kLLVMCVariableEditsProjectionProofWorkLimit) {
          Flow.Refusal = "The storage alias proof exceeded its budget";
          return;
        }
        const auto &T = Slots[U];
        if (S.Allocation == T.Allocation &&
            S.Offset < T.Offset + Flow.Variables[U].Bytes &&
            T.Offset < S.Offset + Flow.Variables[V].Bytes) {
          refuse(V, "The stack byte intervals overlap");
          refuse(U, "The stack byte intervals overlap");
        }
      }
      // Every pointer use must be an exact constant access. An escaping
      // address, dynamic index, memory intrinsic or unmodelled operation
      // refuses the allocation.
      std::vector<const llvm::Value *> Work{S.Allocation};
      std::set<const llvm::Value *> Seen;
      while (!Work.empty()) {
        const auto *P = Work.back();
        Work.pop_back();
        if (!Seen.insert(P).second)
          continue;
        if (Seen.size() > ::neverd::limits::backend::
                              kLLVMCVariableEditsProjectionSeenSizeLimit) {
          refuse(V, "The address-use proof exceeded its budget");
          break;
        }
        for (const auto *User : P->users()) {
          if (++ProofWork > ::neverd::limits::backend::
                                kLLVMCVariableEditsProjectionProofWorkLimit) {
            Flow.Refusal = "The storage alias proof exceeded its budget";
            return;
          }
          if (const auto *L = llvm::dyn_cast<llvm::LoadInst>(User);
              L && L->getPointerOperand() == P) {
            if (!storage(P) || Flow.Nodes[Nodes.at(L)].Reads.empty())
              refuse(V, "The allocation has an unmodelled or aggregate read");
          } else if (const auto *W = llvm::dyn_cast<llvm::StoreInst>(User);
                     W && W->getPointerOperand() == P &&
                     W->getValueOperand() != P) {
            if (!storage(P) || Flow.Nodes[Nodes.at(W)].Writes.empty())
              refuse(V, "The allocation has an unmodelled or aggregate write");
          } else if (llvm::isa<llvm::GetElementPtrInst, llvm::BitCastInst>(
                         User) &&
                     storage(User))
            Work.push_back(User);
          else if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(User);
                   Call &&
                   (Call->getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
                    Call->getIntrinsicID() == llvm::Intrinsic::lifetime_end)) {
            // Lifetime hints do not expose the bytes.
          } else
            refuse(V, "The local address escapes, is dynamically indexed or "
                      "has an unsupported use");
        }
      }
    }
    // A load used by a store copies an entire value only if no write to the
    // source slot occurs between them in the same basic block.
    for (auto &S : Slots)
      for (auto [I, N] : S.Accesses)
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(I)) {
          const auto *Load =
              llvm::dyn_cast<llvm::LoadInst>(Store->getValueOperand());
          if (!Load || Load->getParent() != Store->getParent() ||
              !Load->comesBefore(Store))
            continue;
          const auto Source = storage(Load->getPointerOperand());
          if (!Source || !Locations.count(*Source))
            continue;
          const auto From = Locations.at(*Source),
                     To = Flow.Nodes[N].Writes.front();
          bool Killed = false;
          for (const auto *Between = Load->getNextNode(); Between != Store;
               Between = Between->getNextNode())
            Killed |=
                std::find(Flow.Nodes[Nodes.at(Between)].Writes.begin(),
                          Flow.Nodes[Nodes.at(Between)].Writes.end(),
                          From) != Flow.Nodes[Nodes.at(Between)].Writes.end();
          if (!Killed) {
            Flow.Nodes[N].Reads.push_back(From);
            Flow.Nodes[N].Copy = {{To, From}};
          }
        }
  }
  void refuse(unsigned V, std::string Reason) {
    Flow.Variables[V].Eligible = false;
    Flow.Variables[V].Refusal = std::move(Reason);
  }
};
} // namespace
std::string neverd::llvmc::sourceValueId(const llvm::Instruction &I,
                                         unsigned Index) {
  if (const auto *Node = I.getMetadata(Identity);
      Node && Node->getNumOperands() == 1)
    if (const auto *Text = llvm::dyn_cast<llvm::MDString>(Node->getOperand(0)))
      return Text->getString().str();
  return "instruction:" + std::to_string(Index);
}
void neverd::llvmc::captureSourceValueIds(llvm::Function &Function) {
  unsigned Index = 0;
  for (auto &Block : Function)
    for (auto &I : Block) {
      const auto Id = sourceValueId(I, Index++);
      I.setMetadata(Identity,
                    llvm::MDNode::get(I.getContext(),
                                      llvm::MDString::get(I.getContext(), Id)));
    }
}
std::string neverd::llvmc::sourceStorageId(const llvm::Value *Pointer,
                                           unsigned Bytes) {
  const auto Location = storage(Pointer);
  if (!Location)
    return {};
  const auto &[Allocation, Offset] = *Location;
  unsigned Index = 0;
  for (const auto &B : *Allocation->getFunction())
    for (const auto &I : B) {
      if (&I == Allocation) {
        const auto Id = sourceValueId(I, Index);
        if (!Offset && scalar(Allocation->getAllocatedType()) &&
            Allocation->getModule()->getDataLayout().getTypeStoreSize(
                Allocation->getAllocatedType()) == Bytes)
          return Id;
        return "storage:" + Id + ":" + std::to_string(Offset) + ":" +
               std::to_string(Bytes);
      }
      ++Index;
    }
  return {};
}
SourceVariableEditResult
neverd::llvmc::editVariableStorage(llvm::Function &Function,
                                   const SourceVariableEdit &Edit,
                                   unsigned EditIndex) {
  SourceVariableEditResult Result;
  Projection P(Function);
  Result.Refusal = P.Flow.Refusal;
  if (!Result.Refusal.empty())
    return Result;
  const auto From = P.Ids.find(Edit.From), To = P.Ids.find(Edit.To);
  if (From == P.Ids.end() || (Edit.Kind == "map" && To == P.Ids.end())) {
    Result.Refusal = "LLVM SSA values have one definition; select complete "
                     "local scalar storage for an association";
    return Result;
  }
  SourceVariableSplit Split;
  if (Edit.Kind == "map")
    Result.Refusal = canMapSourceVariables(P.Flow, From->second, To->second);
  else {
    Split = splitSourceVariable(P.Flow, From->second);
    Result.Refusal = Split.Refusal;
  }
  if (!Result.Refusal.empty())
    return Result;
  const unsigned Count = Edit.Kind == "map" ? 1 : Split.Partitions;
  auto &Selected = P.Slots[From->second];
  std::vector<llvm::Value *> Targets;
  llvm::IRBuilder<> Builder(&Function.getEntryBlock().front());
  for (unsigned I = 0; I < Count; ++I) {
    const auto Id =
        "association:" + std::to_string(EditIndex) + ":" + std::to_string(I);
    auto *Allocation =
        Builder.CreateAlloca(Selected.Type, nullptr, "local_part");
    Allocation->setMetadata(
        Identity,
        llvm::MDNode::get(Function.getContext(),
                          llvm::MDString::get(Function.getContext(), Id)));
    Allocation->setMetadata("neverd.source.association",
                            llvm::MDNode::get(Function.getContext(), {}));
    Result.Outputs.push_back(Id);
    Targets.push_back(Allocation);
  }
  auto Rewrite = [&](const Slot &S) {
    for (const auto &[Instruction, N] : S.Accesses) {
      const bool Write = llvm::isa<llvm::StoreInst>(Instruction);
      const auto Part = Edit.Kind == "map"
                            ? 0u
                            : (Write ? Split.Definitions[N] : Split.Uses[N]);
      if (Part == SourceVariableSplit::None)
        continue;
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(Instruction))
        L->setOperand(0, Targets[Part]);
      else
        llvm::cast<llvm::StoreInst>(Instruction)->setOperand(1, Targets[Part]);
    }
  };
  Rewrite(Selected);
  if (Edit.Kind == "map")
    Rewrite(P.Slots[To->second]);
  Result.Applied = true;
  return Result;
}
