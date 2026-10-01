//===- LLVMInterpreterMachineState.cpp - Scalar source model --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMInterpreterModelInternal.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"

#include <limits>

namespace neverd::analysis::llvm_model {

void Builder::charge(uint64_t &Counter, uint64_t Amount, uint64_t Limit) {
  if (Amount > Limit || Counter > Limit - Amount)
    throw Failure{"LLVM machine-state model budget exhausted", true};
  Counter += Amount;
}
NdVar Builder::local(unsigned Bytes) {
  work();
  if (NextTemporary >= (uint64_t{1} << 38))
    fail("LLVM temporary namespace exhausted");
  NdVar V = NdVar::tmp(NextTemporary, Bytes);
  NextTemporary += 8;
  return V;
}
NdVar Builder::fresh(unsigned Bytes) {
  work();
  if (NextRegister >= (uint64_t{1} << 41))
    fail("LLVM register namespace exhausted");
  NdVar V = rvar(NextRegister, Bytes);
  NextRegister += 8;
  return V;
}
int Builder::block() {
  work();
  auto &Blocks = Result.Function.Blocks;
  if (Blocks.size() >= Limits.MaxBlocks ||
      Blocks.size() >= static_cast<size_t>(std::numeric_limits<int>::max()))
    throw Failure{"LLVM machine-state block budget exhausted", true};
  LowBlock B;
  B.Id = static_cast<int>(Blocks.size());
  B.StartAddr = 0x1000 + Blocks.size() * 0x100;
  B.EndAddr = B.StartAddr + 1;
  Blocks.push_back(std::move(B));
  return Blocks.back().Id;
}
void Builder::emit(LowBlock &Block, LowOp Operation) {
  charge(Operations, 1, Limits.MaxOperations);
  work();
  if (Block.Ops.size() >= static_cast<size_t>(std::numeric_limits<int>::max()))
    throw Failure{"LLVM instruction operation budget exhausted", true};
  Block.Ops.push_back(Operation);
}

void Builder::preflight() {
  if (!F.getParent() || F.isDeclaration() || F.isVarArg() ||
      F.arg_size() != 1 || !F.getArg(0)->getType()->isPointerTy() ||
      !F.getReturnType()->isIntegerTy(64))
    fail("LLVM model requires one state pointer and an i64 status result");
  const auto &DL = F.getParent()->getDataLayout();
  if (F.getParent()->getDataLayoutStr().empty() || !DL.isLittleEndian() ||
      DL.getPointerSizeInBits(0) != 64 || DL.getIndexSizeInBits(0) != 64 ||
      DL.isNonIntegralAddressSpace(0) ||
      F.getArg(0)->getType()->getPointerAddressSpace() != 0)
    fail("unsupported LLVM machine-state data layout");
  input(F.size());
  input(F.getParent()->getDataLayoutStr().size());
  for (auto Set : F.getAttributes())
    for (auto A : Set) {
      input();
      if (A.isStringAttribute()) {
        input(A.getKindAsString().size());
        input(A.getValueAsString().size());
      }
    }
  std::set<const llvm::Metadata *> Seen;
  std::vector<const llvm::Metadata *> Pending;
  auto Metadata = [&](const auto &Object) {
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> Attachments;
    Object.getAllMetadata(Attachments);
    input(Attachments.size());
    for (auto [Kind, Node] : Attachments) {
      (void)Kind;
      Pending.push_back(Node);
    }
  };
  Metadata(F);
  for (const auto &B : F) {
    if (B.empty() || !B.back().isTerminator())
      fail("LLVM block has no terminator");
    input(B.size());
    for (const auto &I : B) {
      input(I.getNumOperands());
      for (const auto *V : I.operand_values()) {
        if (llvm::isa<llvm::Constant>(V) &&
            !llvm::isa<llvm::ConstantInt, llvm::Function>(V))
          fail("unsupported LLVM constant, undef or poison operand");
        if (llvm::isa<llvm::ConstantInt>(V))
          bytes(V->getType());
      }
      Metadata(I);
    }
  }
  while (!Pending.empty()) {
    auto *M = Pending.back();
    Pending.pop_back();
    work();
    if (!M || !Seen.insert(M).second)
      continue;
    input();
    if (auto *String = llvm::dyn_cast<llvm::MDString>(M))
      input(String->getString().size());
    if (auto *Node = llvm::dyn_cast<llvm::MDNode>(M)) {
      input(Node->getNumOperands());
      for (const auto &Operand : Node->operands())
        Pending.push_back(Operand.get());
    }
  }
  if (llvm::verifyFunction(F))
    fail("invalid LLVM function");
}
bool Builder::blockLocal(const llvm::Instruction &I) {
  if (llvm::isa<llvm::PHINode>(I))
    return false;
  for (auto *U : I.users()) {
    work();
    auto *Use = llvm::dyn_cast<llvm::Instruction>(U);
    if (!Use || Use->getParent() != I.getParent() ||
        llvm::isa<llvm::PHINode, llvm::SwitchInst>(Use))
      return false;
  }
  return true;
}
unsigned Builder::bytes(llvm::Type *T) {
  if (T->isPointerTy() && T->getPointerAddressSpace() == 0)
    return 8;
  if (!T->isIntegerTy())
    fail("noninteger type unsupported");
  unsigned N = T->getIntegerBitWidth();
  if (N != 1 && N != 8 && N != 16 && N != 32 && N != 64)
    fail("integer width unsupported");
  return (N + 7) / 8;
}
NdVar Builder::value(const llvm::Value *V) {
  if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(V))
    fail("undef or poison unsupported");
  if (auto *K = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    auto Bytes = bytes(K->getType());
    return num(K->getZExtValue(), Bytes);
  }
  auto I = Values.find(V);
  if (I == Values.end())
    fail("unbound LLVM value");
  return I->second;
}
int Builder::target(const llvm::BasicBlock *From, const llvm::BasicBlock *To) {
  auto I = Edges.find({From, To});
  return I == Edges.end() ? Blocks.at(To) : I->second;
}

void Builder::createGraph() {
  for (const auto &B : F) {
    Blocks[&B] = block();
    for (const auto &I : B) {
      work();
      if (I.getType()->isVoidTy() || StateOffsets.count(&I))
        continue;
      auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
      auto ID = Call && Call->getCalledFunction()
                    ? Call->getCalledFunction()->getIntrinsicID()
                    : llvm::Intrinsic::not_intrinsic;
      if (I.getType()->isStructTy() &&
          (ID == llvm::Intrinsic::sadd_with_overflow ||
           ID == llvm::Intrinsic::ssub_with_overflow)) {
        auto Bytes = bytes(Call->getArgOperand(0)->getType());
        Aggregates[&I] = blockLocal(I) ? std::make_pair(local(Bytes), local(1))
                                       : std::make_pair(fresh(Bytes), fresh(1));
      } else
        Values[&I] = blockLocal(I) ? local(bytes(I.getType()))
                                   : fresh(bytes(I.getType()));
    }
  }
  for (const auto &B : F) {
    if (auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(B.getTerminator())) {
      auto &Dispatch = SwitchBlocks[Switch];
      Dispatch.push_back(Blocks.at(&B));
      for (unsigned I = 1; I < Switch->getNumCases(); ++I)
        Dispatch.push_back(block());
    }
  }
  Result.Function.Entry = address(Blocks.at(&F.front()));
  Result.Function.Name = "llvm_machine_state";
  for (const auto &B : F) {
    const auto *T = B.getTerminator();
    for (unsigned I = 0; I != T->getNumSuccessors(); ++I) {
      work();
      const auto *To = T->getSuccessor(I);
      if (To->phis().empty())
        continue;
      auto Key = std::make_pair(&B, To);
      if (!Edges.count(Key)) {
        Edges[Key] = block();
        EdgeOrder.push_back(Key);
      }
    }
  }
}

void Builder::emitEdges() {
  for (const auto &Key : EdgeOrder) {
    auto &B = Result.Function.Blocks[Edges.at(Key)];
    std::vector<std::pair<NdVar, NdVar>> Copies;
    for (const auto &Phi : Key.second->phis()) {
      auto Temp = local(bytes(Phi.getType()));
      emit(B, op(NdOp::COPY, Temp,
                 {value(Phi.getIncomingValueForBlock(Key.first))}));
      Copies.push_back({value(&Phi), Temp});
    }
    // Capture every incoming value before writing any destination PHI.
    for (auto [Dest, Temp] : Copies)
      emit(B, op(NdOp::COPY, Dest, {Temp}));
    B.Succs = {Blocks.at(Key.second)};
    emit(B, op(NdOp::BRANCH, {}, {num(address(B.Succs[0]))}));
  }
}

void Builder::seal() {
  for (auto &B : Result.Function.Blocks) {
    if (B.Ops.empty())
      fail("empty generated LLVM model block");
    LowInstructionBoundary IB;
    IB.Address = B.StartAddr;
    IB.Size = 1;
    IB.OpCount = B.Ops.size();
    for (size_t N = 0; N != B.Ops.size(); ++N) {
      auto &O = B.Ops[N];
      O.Addr = IB.Address;
      O.Seq = static_cast<int>(N);
      if (O.Opcode == NdOp::BRANCH || O.Opcode == NdOp::COND_BR) {
        IB.Control = LowInstructionControl::Branch;
        IB.ControlFlags = LowInstructionControlFlag::Branch;
        IB.Immediate = O.Inputs[0].Offset;
        if (O.Opcode == NdOp::COND_BR)
          IB.ControlFlags |= LowInstructionControlFlag::Conditional;
      } else if (O.Opcode == NdOp::RETURN) {
        IB.Control = LowInstructionControl::Return;
        IB.ControlFlags = LowInstructionControlFlag::Return;
      }
    }
    B.InstructionBoundaries = {IB};
    LowInstructionUndefinedEffects E;
    E.Coverage = LowUndefinedCoverage::Complete;
    E.OpCount = B.Ops.size();
    E.OperationDigest = lowUndefinedOperationDigest(B.Ops);
    Result.Instructions.push_back({B.Id, IB, std::move(E)});
    for (int Successor : B.Succs)
      Result.Function.Blocks[Successor].Preds.push_back(B.Id);
  }
  if (auto Error = validateLowInstructionBoundaries(
          Result.Function, LowInstructionBoundaryRequirement::Required))
    fail(llvm::toString(std::move(Error)));
}

InterpreterMachineStateModel Builder::build() {
  preflight();
  pointerProjections();
  validateContract();
  createGraph();
  emitEdges();
  for (const auto &B : F) {
    auto &Out = Result.Function.Blocks[Blocks.at(&B)];
    for (const auto &I : B) {
      work();
      if (llvm::isa<llvm::PHINode>(I) || StateOffsets.count(&I))
        continue;
      if (!emitMemory(Out, I) && !emitScalar(Out, I) && !emitControl(Out, I))
        fail(llvm::Twine("unsupported LLVM instruction: ") + I.getOpcodeName());
    }
  }
  seal();
  return std::move(Result);
}
} // namespace neverd::analysis::llvm_model

namespace neverd::analysis {
LowIRIndependenceContract llvmInterpreterMachineStateContract() {
  LowIRIndependenceContract C;
  for (unsigned Offset = 0; Offset != sizeof(InterpreterMachineStateX64V1);
       Offset += 8)
    C.ReturnRegisters.push_back({Offset, 8});
  C.EntryConstants.push_back(
      {NdVar::reg(LLVMInterpreterDefinednessOffset, 1), 0});
  C.ReturnRegisters.push_back({LLVMInterpreterDefinednessOffset, 1});
  C.PreservedRegisters.push_back({LLVMInterpreterDefinednessOffset, 1});
  return C;
}
llvm::Expected<InterpreterMachineStateModel>
modelLLVMInterpreterMachineStateX64(const llvm::Function &Function,
                                    const LLVMInterpreterModelLimits &Limits) {
  try {
    return llvm_model::Builder(Function, Limits).build();
  } catch (const llvm_model::Failure &E) {
    return llvm::createStringError(E.Budget ? llvm::errc::result_out_of_range
                                            : llvm::errc::invalid_argument,
                                   "%s", E.Message.c_str());
  }
}
} // namespace neverd::analysis
