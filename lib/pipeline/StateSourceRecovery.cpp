//===- StateSourceRecovery.cpp - Project explicit state ABI contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep unknown boundaries intact while exposing proven scalar source calls.

#include "neverd/pipeline/StateSourceRecovery.h"

#include "neverd/backend/llvm/LLVMSourceTypeHints.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <map>
#include <set>

namespace neverd {
namespace {
constexpr llvm::StringLiteral ProbePrefix = "__neverd_state_probe_";
llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "state source contract: " + Message);
}
llvm::Type *type(llvm::LLVMContext &C, StateSourceType T) {
  switch (T) {
  case StateSourceType::Void:
    return llvm::Type::getVoidTy(C);
  case StateSourceType::U32:
  case StateSourceType::I32:
    return llvm::Type::getInt32Ty(C);
  case StateSourceType::U64:
  case StateSourceType::I64:
    return llvm::Type::getInt64Ty(C);
  case StateSourceType::Pointer:
    return llvm::PointerType::getUnqual(C);
  }
  llvm_unreachable("invalid source type");
}
bool signedType(StateSourceType T) {
  return T == StateSourceType::I32 || T == StateSourceType::I64;
}
bool identifier(llvm::StringRef Name) {
  return !Name.empty() && Name.size() <= 128 &&
         (llvm::isAlpha(Name.front()) || Name.front() == '_') &&
         llvm::all_of(Name,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; }) &&
         !llvm::StringSwitch<bool>(Name)
              .Cases({"auto",         "break",     "case",
                      "char",         "const",     "continue",
                      "default",      "do",        "double",
                      "else",         "enum",      "extern",
                      "float",        "for",       "goto",
                      "if",           "inline",    "int",
                      "long",         "register",  "restrict",
                      "return",       "short",     "signed",
                      "sizeof",       "static",    "struct",
                      "switch",       "typedef",   "union",
                      "unsigned",     "void",      "volatile",
                      "while",        "alignas",   "alignof",
                      "bool",         "constexpr", "nullptr",
                      "false",        "true",      "static_assert",
                      "thread_local", "typeof",    "typeof_unqual",
                      "_Alignas",     "_Alignof",  "_Atomic",
                      "_Bool",        "_Complex",  "_Generic",
                      "_Imaginary",   "_Noreturn", "_Static_assert",
                      "_Thread_local"},
                     true)
              .Default(false);
}
llvm::FunctionType *signature(llvm::LLVMContext &C,
                              const StateSourceImport &I) {
  std::vector<llvm::Type *> Parameters;
  for (auto T : I.Parameters)
    Parameters.push_back(type(C, T));
  return llvm::FunctionType::get(type(C, I.Result), Parameters, false);
}

// Exact pointer-sized arithmetic only. Loading a pointer from the bank is a
// guest access, not an address of the bank. Never chase that load as a base.
struct Offsets {
  std::vector<int64_t> Bank;
  bool Guest = false;
};
std::optional<Offsets> bankOffsets(llvm::Value *V, llvm::Value *Base,
                                   const llvm::DataLayout &DL,
                                   unsigned &Budget) {
  if (!Budget)
    return {};
  --Budget;
  if (V == Base)
    return Offsets{{0}, false};
  if (llvm::isa<llvm::LoadInst, llvm::Argument, llvm::Constant>(V))
    return Offsets{{}, true};
  auto Next = [&](llvm::Value *X) { return bankOffsets(X, Base, DL, Budget); };
  if (auto *C = llvm::dyn_cast<llvm::CastInst>(V)) {
    if (C->getOpcode() == llvm::Instruction::BitCast)
      return Next(C->getOperand(0));
    if (C->getOpcode() == llvm::Instruction::PtrToInt &&
        C->getType()->getIntegerBitWidth() == DL.getPointerSizeInBits(0))
      return Next(C->getOperand(0));
    if (C->getOpcode() == llvm::Instruction::IntToPtr &&
        C->getType()->getPointerAddressSpace() == 0 &&
        C->getOperand(0)->getType()->getIntegerBitWidth() ==
            DL.getPointerSizeInBits(0))
      return Next(C->getOperand(0));
    return {};
  }
  auto Shift = [](std::optional<Offsets> Values, int64_t Amount) {
    if (Values)
      for (auto &O : Values->Bank)
        O += Amount;
    return Values;
  };
  if (auto *G = llvm::dyn_cast<llvm::GetElementPtrInst>(V)) {
    llvm::APInt Offset(DL.getIndexSizeInBits(0), 0);
    if (G->getAddressSpace() == 0 && G->accumulateConstantOffset(DL, Offset) &&
        Offset.getSignificantBits() <= 32)
      return Shift(Next(G->getPointerOperand()), Offset.getSExtValue());
    return {};
  }
  if (auto *B = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
    if (B->getOpcode() != llvm::Instruction::Add &&
        B->getOpcode() != llvm::Instruction::Sub)
      return {};
    auto *K = llvm::dyn_cast<llvm::ConstantInt>(B->getOperand(1));
    auto *Source = B->getOperand(0);
    if (!K && B->getOpcode() == llvm::Instruction::Add) {
      K = llvm::dyn_cast<llvm::ConstantInt>(B->getOperand(0));
      Source = B->getOperand(1);
    }
    if (K && K->getValue().getSignificantBits() <= 32)
      return Shift(Next(Source), B->getOpcode() == llvm::Instruction::Sub
                                     ? -K->getSExtValue()
                                     : K->getSExtValue());
    return {};
  }
  std::vector<llvm::Value *> Arms;
  if (auto *P = llvm::dyn_cast<llvm::PHINode>(V))
    for (auto &A : P->incoming_values())
      Arms.push_back(A.get());
  if (auto *S = llvm::dyn_cast<llvm::SelectInst>(V))
    Arms = {S->getTrueValue(), S->getFalseValue()};
  if (Arms.empty())
    return {};
  Offsets Result;
  for (auto *A : Arms) {
    auto Values = Next(A);
    if (!Values)
      return {};
    Result.Guest |= Values->Guest;
    for (auto O : Values->Bank)
      if (!llvm::is_contained(Result.Bank, O))
        Result.Bank.push_back(O);
    if (Result.Bank.size() > 32)
      return {};
  }
  return Result;
}
std::optional<Offsets> bankOffsets(llvm::Value *V, llvm::Value *Base,
                                   const llvm::DataLayout &DL) {
  unsigned Budget = 128;
  return bankOffsets(V, Base, DL, Budget);
}
std::optional<int64_t> displacement(llvm::Value *V, llvm::Value *Base,
                                    const llvm::DataLayout &DL) {
  auto Values = bankOffsets(V, Base, DL);
  if (Values && !Values->Guest && Values->Bank.size() == 1)
    return Values->Bank.front();
  return {};
}
// Only a complete, ordinary pointer-width load supplies slot provenance.
// This identifies a specialization candidate, not the loaded value. A later
// runtime address comparison remains mandatory, including after intervening
// guest writes or calls. Do not walk through narrow carriers or merged values.
llvm::LoadInst *pointerLoad(llvm::Value *V) {
  while (auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
    if (Cast->getOpcode() != llvm::Instruction::BitCast &&
        Cast->getOpcode() != llvm::Instruction::PtrToInt &&
        Cast->getOpcode() != llvm::Instruction::IntToPtr)
      return nullptr;
    auto Valid = [](llvm::Type *T) {
      return T->isIntegerTy(64) ||
             (T->isPointerTy() && T->getPointerAddressSpace() == 0);
    };
    if (!Valid(Cast->getType()) || !Valid(Cast->getOperand(0)->getType()))
      return nullptr;
    V = Cast->getOperand(0);
  }
  auto *Load = llvm::dyn_cast<llvm::LoadInst>(V);
  if (!Load || Load->isAtomic() || Load->isVolatile() ||
      (!Load->getType()->isIntegerTy(64) &&
       !(Load->getType()->isPointerTy() &&
         Load->getType()->getPointerAddressSpace() == 0)))
    return nullptr;
  return Load;
}
// Bank transport across an opaque call does not prove the current value.
// Follow bounded dominating stores only to choose a candidate; the actual
// captured target and arity are still checked before invoking that candidate.
llvm::LoadInst *pointerCandidateLoad(llvm::Value *V, llvm::Value *Bank,
                                     llvm::DominatorTree &DT,
                                     const llvm::DataLayout &DL) {
  unsigned Budget = 4096;
  while (Budget) {
    --Budget;
    auto *Load = pointerLoad(V);
    if (!Load)
      return nullptr;
    auto Offset = displacement(Load->getPointerOperand(), Bank, DL);
    if (!Offset)
      return Load;
    auto *Block = Load->getParent();
    auto Position = Load->getIterator();
    llvm::Value *Stored = nullptr;
    while (Block && Budget && !Stored) {
      while (Position != Block->begin() && Budget) {
        --Position;
        --Budget;
        auto *Store = llvm::dyn_cast<llvm::StoreInst>(&*Position);
        if (!Store || Store->isAtomic() || Store->isVolatile() ||
            displacement(Store->getPointerOperand(), Bank, DL) != Offset ||
            DL.getTypeStoreSize(Store->getValueOperand()->getType()) != 8)
          continue;
        Stored = Store->getValueOperand();
        break;
      }
      auto *Node = DT.getNode(Block);
      Block = Node && Node->getIDom() ? Node->getIDom()->getBlock() : nullptr;
      if (Block)
        Position = Block->end();
    }
    if (!Stored)
      return nullptr;
    V = Stored;
  }
  return nullptr;
}
bool pointerCandidateMatches(llvm::LoadInst &Origin, llvm::Value *Bank,
                             const StateSourceImage &Image, unsigned Offset,
                             const llvm::DataLayout &DL) {
  unsigned Budget = 4096;
  for (auto &I : llvm::instructions(Origin.getFunction())) {
    if (!Budget--)
      return false;
    auto *Base = llvm::dyn_cast<llvm::LoadInst>(&I);
    if (!Base || pointerLoad(Base) != Base ||
        displacement(Base->getPointerOperand(), Bank, DL) != Image.Base.Offset)
      continue;
    // A past bank base is advisory too: an opaque call may have replaced it.
    // No extra guest load is generated, and only target equality permits the
    // native call. Candidate bytes and sampled addresses are never consulted.
    if (displacement(Origin.getPointerOperand(), Base, DL) == Offset)
      return true;
  }
  return false;
}
llvm::Value *slot(llvm::IRBuilder<> &B, llvm::Value *Bank, unsigned Offset) {
  return B.CreateConstGEP1_64(B.getInt8Ty(), Bank, Offset, "state_slot");
}
llvm::Value *read(llvm::IRBuilder<> &B, llvm::Value *Bank, StateSourceSlot S,
                  llvm::MDNode *Scope) {
  auto *V = B.CreateAlignedLoad(B.getIntNTy(S.Bytes * 8),
                                slot(B, Bank, S.Offset), llvm::Align(1));
  V->setMetadata(llvm::LLVMContext::MD_alias_scope, Scope);
  return B.CreateZExtOrTrunc(V, B.getInt64Ty());
}
void write(llvm::IRBuilder<> &B, llvm::Value *Bank, StateSourceSlot S,
           llvm::Value *V, llvm::MDNode *Scope) {
  V = B.CreateZExtOrTrunc(V, B.getIntNTy(S.Bytes * 8));
  B.CreateAlignedStore(V, slot(B, Bank, S.Offset), llvm::Align(1))
      ->setMetadata(llvm::LLVMContext::MD_alias_scope, Scope);
}
llvm::MDNode *bankScope(llvm::Function &F) {
  llvm::MDBuilder B(F.getContext());
  auto *Domain = B.createAnonymousAliasScopeDomain("source register bank");
  return llvm::MDNode::get(F.getContext(), B.createAnonymousAliasScope(Domain));
}

// Admit bounded bank accesses before attaching isolation facts. Address
// observation or escape must not turn an ambiguous access into guest memory.
llvm::Error validateBank(llvm::Function &F, unsigned Bytes) {
  auto *Base = F.getArg(0);
  const auto &DL = F.getParent()->getDataLayout();
  std::vector<llvm::Value *> Work{Base};
  std::set<llvm::Value *> Seen{Base};
  while (!Work.empty()) {
    auto *V = Work.back();
    Work.pop_back();
    for (auto &Use : V->uses()) {
      auto *I = llvm::dyn_cast<llvm::Instruction>(Use.getUser());
      if (!I)
        return invalid("bank address escapes the function");
      llvm::Type *T = nullptr;
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(I)) {
        if (L->isVolatile() || L->isAtomic())
          return invalid("ordered bank access");
        T = L->getType();
      } else if (auto *S = llvm::dyn_cast<llvm::StoreInst>(I)) {
        if (Use.getOperandNo() != 1 || S->isVolatile() || S->isAtomic())
          return invalid("bank address escapes or has ordered access");
        T = S->getValueOperand()->getType();
      } else if (auto *Call = llvm::dyn_cast<llvm::CallInst>(I)) {
        auto Offset = displacement(V, Base, DL);
        if (Use.getOperandNo() != 0 || Offset != 0 ||
            !V->getType()->isPointerTy() || !Call->getType()->isIntegerTy(64) ||
            Call->isMustTailCall() || Call->hasOperandBundles() ||
            Call->getCallingConv() != llvm::CallingConv::C ||
            Call->getFunctionType()->isVarArg())
          return invalid("unsupported bank call boundary");
        if (auto *Callee = Call->getCalledFunction())
          if (Callee->getFunctionType() != Call->getFunctionType())
            return invalid("inconsistent gateway ABI");
        continue;
      } else if (bankOffsets(I, Base, DL)) {
        if (Seen.insert(I).second)
          Work.push_back(I);
        continue;
      } else {
        std::string Detail;
        llvm::raw_string_ostream OS(Detail);
        OS << "bank address is observed or has a dynamic offset in "
           << F.getName() << ": ";
        I->print(OS);
        return invalid(Detail);
      }
      auto Offsets = bankOffsets(V, Base, DL);
      if (!Offsets || DL.getTypeStoreSize(T).isScalable())
        return invalid("bank access has an unbounded address or width");
      for (auto Offset : Offsets->Bank)
        if (Offset < 0 || uint64_t(Offset) > Bytes ||
            DL.getTypeStoreSize(T).getFixedValue() > Bytes - Offset)
          return invalid("bank access outside declared storage");
    }
  }
  return llvm::Error::success();
}

// A wide load may preserve unknown upper bytes for another state store, while
// an import selector uses only its low word. GVN cannot discard the wide use.
// Expose a separate narrow load at the ORIGINAL read, never at its later use:
// the ordinary memory optimizer can then forward an exact partial writer.
bool exposeBankLoadSlices(llvm::Module &M) {
  struct Slice {
    llvm::Instruction *Use;
    llvm::LoadInst *Load;
    unsigned Bits;
  };
  std::vector<Slice> Slices;
  for (auto &F : M) {
    if (F.isDeclaration() || F.arg_empty() ||
        !F.getArg(0)->getType()->isPointerTy())
      continue;
    for (auto &I : llvm::instructions(F)) {
      llvm::LoadInst *L = nullptr;
      unsigned Bits = 0;
      if (auto *T = llvm::dyn_cast<llvm::TruncInst>(&I)) {
        L = llvm::dyn_cast<llvm::LoadInst>(T->getOperand(0));
        if (T->getType()->isIntegerTy())
          Bits = T->getType()->getIntegerBitWidth();
      } else if (I.getOpcode() == llvm::Instruction::And) {
        for (unsigned Side : {0u, 1u}) {
          auto *Mask =
              llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(1 - Side));
          if (Mask && Mask->getValue().isMask()) {
            L = llvm::dyn_cast<llvm::LoadInst>(I.getOperand(Side));
            Bits = Mask->getValue().getActiveBits();
            break;
          }
        }
      }
      if (!L || L->isVolatile() || L->isAtomic() ||
          !L->getType()->isIntegerTy() ||
          L->getType()->getIntegerBitWidth() > 64 || !Bits || Bits % 8 ||
          Bits >= L->getType()->getIntegerBitWidth() ||
          !displacement(L->getPointerOperand(), F.getArg(0), M.getDataLayout()))
        continue;
      if (Slices.size() < 4096)
        Slices.push_back({&I, L, Bits});
    }
  }
  for (auto [Use, L, Bits] : Slices) {
    llvm::IRBuilder<> B(L);
    auto *Pointer = L->getPointerOperand();
    if (M.getDataLayout().isBigEndian())
      Pointer =
          slot(B, Pointer, (L->getType()->getIntegerBitWidth() - Bits) / 8);
    auto *Narrow = B.CreateAlignedLoad(B.getIntNTy(Bits), Pointer,
                                       llvm::Align(1), "state_value");
    Narrow->setMetadata(llvm::LLVMContext::MD_alias_scope,
                        L->getMetadata(llvm::LLVMContext::MD_alias_scope));
    Narrow->setMetadata(llvm::LLVMContext::MD_noalias,
                        L->getMetadata(llvm::LLVMContext::MD_noalias));
    Use->replaceAllUsesWith(B.CreateZExtOrTrunc(Narrow, Use->getType()));
    Use->eraseFromParent();
  }
  return !Slices.empty();
}

} // namespace

llvm::Expected<StateSourceRecovery>
prepareStateSourceRecovery(llvm::Module &M, const StateSourceContract &C) {
  if (!C.IsolatedBank || (!C.Gateways.empty() && !C.ZeroStatusGateways) ||
      !C.StateBytes || C.StateBytes > 65536 || C.Gateways.size() > 256)
    return invalid(
        "requires a bounded isolated bank and explicit zero-status gateways");
  if (llvm::verifyModule(M, &llvm::errs()))
    return invalid("invalid input IR");
  const auto &DL = M.getDataLayout();
  if (DL.getPointerSizeInBits(0) != 64 || DL.getIndexSizeInBits(0) != 64 ||
      DL.isNonIntegralAddressSpace(0))
    return invalid("requires integral 64-bit pointers and indices");
  auto ValidSlot = [&](StateSourceSlot S) {
    return (S.Bytes == 1 || S.Bytes == 2 || S.Bytes == 4 || S.Bytes == 8) &&
           S.Offset <= C.StateBytes && S.Bytes <= C.StateBytes - S.Offset;
  };
  if (C.Images.size() > 16)
    return invalid("too many image candidates");
  for (const auto &Image : C.Images)
    if (!Image.Readable || !ValidSlot(Image.Base) || Image.Base.Bytes != 8 ||
        Image.Bytes.empty() || Image.Bytes.size() > 65536)
      return invalid("invalid readable image candidate");
  std::map<std::string, const StateSourceImport *> Names;
  std::map<std::string, size_t> Gateways;
  for (size_t Index = 0; Index < C.Gateways.size(); ++Index) {
    const auto &G = C.Gateways[Index];
    if (!Gateways.emplace(G.Name, Index).second || !identifier(G.Name) ||
        G.Arguments.size() > 16 || G.Imports.size() > 4096 ||
        !ValidSlot(G.ResultOffset) || !ValidSlot(G.ResultBytes) ||
        (G.Selector && !ValidSlot(*G.Selector)) ||
        (G.FunctionPointer && (G.Selector || !ValidSlot(*G.FunctionPointer) ||
                               G.FunctionPointer->Bytes != 8)) ||
        (G.ArgumentCount && !ValidSlot(*G.ArgumentCount)))
      return invalid("invalid gateway layout");
    for (auto A : G.Arguments)
      if (!ValidSlot(A))
        return invalid("argument outside bank");
    std::set<unsigned> Indirect, Strings;
    for (unsigned A : G.IndirectArguments)
      if (A >= G.Arguments.size() || !Indirect.insert(A).second)
        return invalid("invalid indirect argument");
    if (G.Strings.size() > 16 || G.ExcludedArguments.size() > 64)
      return invalid("too many string arguments or exclusions");
    for (const auto &S : G.Strings)
      if (!S.ByValue || S.Argument >= G.Arguments.size() ||
          !Strings.insert(S.Argument).second || S.Image >= C.Images.size() ||
          bool(S.Mode) != bool(S.XorKey) ||
          (S.Mode && (!ValidSlot(*S.Mode) || !ValidSlot(*S.XorKey))))
        return invalid("invalid string normalization contract");
    for (auto E : G.ExcludedArguments)
      if (E.Argument >= G.Arguments.size())
        return invalid("invalid argument exclusion");
    if (G.Writes.size() > 128)
      return invalid("too many fixed writes");
    for (auto W : G.Writes)
      if (!ValidSlot(W.Slot))
        return invalid("write outside bank");
    auto ValidType = [](StateSourceType T) {
      return unsigned(T) <= unsigned(StateSourceType::Pointer);
    };
    std::set<uint64_t> Selectors;
    std::set<std::pair<unsigned, unsigned>> PointerSources;
    for (const auto &I : G.Imports) {
      if (!identifier(I.Name) || Gateways.count(I.Name) ||
          (!G.FunctionPointer && (!Selectors.insert(I.Selector).second ||
                                  (!G.Selector && I.Selector))) ||
          bool(G.FunctionPointer) != bool(I.PointerSource) ||
          (G.FunctionPointer && I.Selector) ||
          I.Parameters.size() > G.Arguments.size() || !ValidType(I.Result) ||
          !llvm::all_of(I.Parameters, ValidType) ||
          llvm::is_contained(I.Parameters, StateSourceType::Void))
        return invalid("invalid import identity or parameters");
      if (I.PointerSource) {
        const auto &P = *I.PointerSource;
        if (P.Image >= C.Images.size() ||
            P.Offset > C.Images[P.Image].Bytes.size() ||
            8 > C.Images[P.Image].Bytes.size() - P.Offset ||
            !PointerSources.emplace(P.Image, P.Offset).second)
          return invalid("invalid or duplicate imported pointer source");
      }
      for (const auto &S : G.Strings)
        if (S.Argument >= I.Parameters.size() ||
            I.Parameters[S.Argument] != StateSourceType::Pointer)
          return invalid("string requires an imported pointer parameter");
      auto *T = signature(M.getContext(), I);
      auto [At, Added] = Names.emplace(I.Name, &I);
      if ((!Added && (At->second->Parameters != I.Parameters ||
                      At->second->Result != I.Result)) ||
          (M.getNamedValue(I.Name) &&
           (!M.getFunction(I.Name) ||
            M.getFunction(I.Name)->getFunctionType() != T ||
            !M.getFunction(I.Name)->isDeclaration())))
        return invalid("conflicting import declaration");
    }
  }
  for (auto &[Name, Index] : Gateways)
    if (Names.count(Name))
      return invalid("import is also a gateway");
  for (auto &F : M) {
    if (F.getName().starts_with(ProbePrefix))
      return invalid("reserved probe name is present");
    if (F.isDeclaration())
      continue;
    if (F.isVarArg() || F.arg_empty() ||
        !F.getArg(0)->getType()->isPointerTy() ||
        F.getArg(0)->getType()->getPointerAddressSpace() != 0 ||
        !F.getReturnType()->isIntegerTy(64))
      return invalid("incompatible state function ABI");
    for (auto &I : llvm::instructions(F))
      if (llvm::isa<llvm::InvokeInst, llvm::CallBrInst>(I))
        return invalid("exceptional state call is unsupported");
    if (auto Error = validateBank(F, C.StateBytes))
      return Error;
  }
  StateSourceRecovery R;
  R.Contract = C;
  R.Probes.resize(C.Gateways.size());
  std::vector<llvm::Function *> Functions;
  for (auto &F : M)
    if (!F.isDeclaration())
      Functions.push_back(&F);
  for (auto *F : Functions) {
    ++R.Functions;
    // Source reconstruction keeps the input's function boundaries. Module
    // inlining obscures the bank interface and can multiply source size.
    F->removeFnAttr(llvm::Attribute::AlwaysInline);
    F->addFnAttr(llvm::Attribute::NoInline);
    auto *Original = F->getArg(0);
    auto *Bank = Original;
    auto *Scope = bankScope(*F);
    std::vector<llvm::CallInst *> Calls;
    for (auto &I : llvm::instructions(F)) {
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
        if (!Call->arg_empty() &&
            displacement(Call->getArgOperand(0), Bank, M.getDataLayout()) == 0)
          Calls.push_back(Call);
        else
          Call->setMetadata(llvm::LLVMContext::MD_noalias, Scope);
      }
      llvm::Value *P = nullptr;
      llvm::Type *T = nullptr;
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        P = L->getPointerOperand();
        T = L->getType();
      }
      if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        P = S->getPointerOperand();
        T = S->getValueOperand()->getType();
      }
      if (P) {
        auto Addresses = bankOffsets(P, Bank, M.getDataLayout());
        if (!Addresses || Addresses->Bank.empty())
          I.setMetadata(llvm::LLVMContext::MD_noalias, Scope);
        else if (!Addresses->Guest)
          I.setMetadata(llvm::LLVMContext::MD_alias_scope, Scope);
      }
      auto Offset = P ? displacement(P, Bank, M.getDataLayout()) : std::nullopt;
      if (!Offset || *Offset < 0 || uint64_t(*Offset) > C.StateBytes ||
          M.getDataLayout().getTypeStoreSize(T).isScalable() ||
          M.getDataLayout().getTypeStoreSize(T).getFixedValue() >
              C.StateBytes - *Offset)
        continue;
      llvm::IRBuilder<> B(&I);
      auto *New = slot(B, Bank, *Offset);
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I))
        L->setOperand(0, New);
      else
        llvm::cast<llvm::StoreInst>(I).setOperand(1, New);
      I.setMetadata(llvm::LLVMContext::MD_alias_scope, Scope);
    }
    for (auto *Call : Calls) {
      llvm::IRBuilder<> B(Call);
      auto *Callee = Call->getCalledFunction();
      auto At =
          Callee ? Gateways.find(Callee->getName().str()) : Gateways.end();
      std::vector<llvm::Value *> Args;
      for (auto &A : Call->args())
        Args.push_back(A.get());
      Args[0] = Original;
      if (At != Gateways.end()) {
        const auto &G = C.Gateways[At->second];
        for (auto S : {G.FunctionPointer ? G.FunctionPointer : G.Selector,
                       G.ArgumentCount, std::optional(G.ResultOffset),
                       std::optional(G.ResultBytes)})
          Args.push_back(S ? read(B, Bank, *S, Scope) : B.getInt64(0));
        if (!G.ArgumentCount)
          Args[Call->arg_size() + 1] = B.getInt64(G.Arguments.size());
        for (auto S : G.Arguments)
          Args.push_back(read(B, Bank, S, Scope));
        for (const auto &S : G.Strings) {
          Args.push_back(read(B, Bank, C.Images[S.Image].Base, Scope));
          Args.push_back(S.Mode ? read(B, Bank, *S.Mode, Scope)
                                : B.getInt64(0));
          Args.push_back(S.XorKey ? read(B, Bank, *S.XorKey, Scope)
                                  : B.getInt64(0));
        }
        std::vector<llvm::Type *> Types;
        for (auto *V : Args)
          Types.push_back(V->getType());
        auto *FT = llvm::FunctionType::get(Call->getType(), Types, false);
        auto &Name = R.Probes[At->second];
        if (Name.empty())
          Name =
              llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage,
                                     ProbePrefix + llvm::Twine(At->second), M)
                  ->getName()
                  .str();
        if (M.getFunction(Name)->getFunctionType() != FT)
          return invalid("inconsistent gateway ABI");
        auto *Replacement =
            B.CreateCall(M.getFunction(Name), Args, "state_call");
        Replacement->setMetadata("neverd.state.scope", Scope);
        Replacement->setMetadata(
            "neverd.state.original_argc",
            llvm::MDNode::get(
                M.getContext(),
                llvm::ConstantAsMetadata::get(B.getInt32(Call->arg_size()))));
        Call->replaceAllUsesWith(Replacement);
        Call->eraseFromParent();
        Call = Replacement;
      } else
        Call->setArgOperand(0, Original);
    }
  }
  return R;
}

llvm::Error finishStateSourceRecovery(llvm::Module &M, StateSourceRecovery &R) {
  // Resolve indirect bank slots only after ordinary propagation has proved
  // their complete offsets. No unchecked pointer arithmetic or speculative
  // bank load is introduced for an unresolved layout.
  bool ResolvedSources = false;
  for (size_t Index = 0; Index < R.Probes.size(); ++Index) {
    auto *Probe = M.getFunction(R.Probes[Index]);
    if (!Probe)
      continue;
    const auto &G = R.Contract.Gateways[Index];
    for (auto *U : Probe->users()) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(U);
      if (!Call || Call->getCalledFunction() != Probe)
        return invalid("probe escaped");
      auto *MD = Call->getMetadata("neverd.state.original_argc");
      auto *Count =
          MD && MD->getNumOperands() == 1
              ? llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(0))
              : nullptr;
      if (!Count)
        return invalid("lost call ABI metadata");
      unsigned N = Count->getZExtValue();
      unsigned Extra = N + 4 + G.Arguments.size();
      if (Call->arg_size() != Extra + 3 * G.Strings.size())
        return invalid("changed probe arity");
      std::vector<std::pair<unsigned, unsigned>> Offsets;
      for (unsigned A : G.IndirectArguments) {
        auto *Offset =
            llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(N + 4 + A));
        if (!Offset || Offset->getZExtValue() > R.Contract.StateBytes ||
            8 > R.Contract.StateBytes - Offset->getZExtValue())
          break;
        Offsets.emplace_back(A, unsigned(Offset->getZExtValue()));
      }
      if (Offsets.size() != G.IndirectArguments.size())
        continue;
      llvm::IRBuilder<> B(Call);
      for (auto [A, Offset] : Offsets)
        Call->setArgOperand(N + 4 + A,
                            read(B, Call->getArgOperand(0), {Offset, 8},
                                 Call->getMetadata("neverd.state.scope")));
      for (unsigned S = 0; S < G.Strings.size(); ++S)
        Call->setArgOperand(
            Extra + 3 * S,
            B.CreateSub(Call->getArgOperand(N + 4 + G.Strings[S].Argument),
                        Call->getArgOperand(Extra + 3 * S), "string_offset"));
      Call->setMetadata("neverd.state.sources_ready",
                        llvm::MDNode::get(M.getContext(), {}));
      ResolvedSources |= !Offsets.empty() || !G.Strings.empty();
    }
  }
  if (ResolvedSources) {
    Pipeline::OptimizationOptions Policy;
    Policy.Strength = Pipeline::OptStrength::Deep;
    Policy.LLVMLevel = llvm::OptimizationLevel::O2;
    auto Result = Pipeline::optimizeModule(M, Policy);
    if (Result.Stop == OptimizationStopReason::InputInvalid ||
        Result.Stop == OptimizationStopReason::VerificationFailed)
      return invalid("argument source optimization failed");
  }
  for (unsigned Round = 0; Round < 3 && exposeBankLoadSlices(M); ++Round) {
    Pipeline::OptimizationOptions Policy;
    Policy.Strength = Pipeline::OptStrength::Deep;
    Policy.LLVMLevel = llvm::OptimizationLevel::O2;
    auto Result = Pipeline::optimizeModule(M, Policy);
    if (Result.Stop == OptimizationStopReason::InputInvalid ||
        Result.Stop == OptimizationStopReason::VerificationFailed)
      return invalid("bank slice optimization failed");
  }
  for (size_t Index = 0; Index < R.Probes.size(); ++Index) {
    if (R.Probes[Index].empty())
      continue;
    auto *Probe = M.getFunction(R.Probes[Index]);
    if (!Probe)
      continue; // The optimizer may delete every unreachable use and
                // declaration.
    const auto &G = R.Contract.Gateways[Index];
    std::vector<llvm::CallInst *> Calls;
    for (auto *U : Probe->users()) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(U);
      if (!Call || Call->getCalledFunction() != Probe)
        return invalid("probe escaped");
      Calls.push_back(Call);
    }
    for (auto *Call : Calls) {
      auto *MD = Call->getMetadata("neverd.state.original_argc");
      auto *Count =
          MD ? llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(0))
             : nullptr;
      if (!Count)
        return invalid("lost call ABI metadata");
      unsigned N = Count->getZExtValue();
      if (Call->arg_size() != N + 4 + G.Arguments.size() + 3 * G.Strings.size())
        return invalid("changed probe arity");
      auto Constant = [&](unsigned Arg) -> std::optional<uint64_t> {
        if (auto *V =
                llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(N + Arg)))
          return V->getZExtValue();
        return {};
      };
      auto Selector = Constant(0), Arity = Constant(1), Offset = Constant(2),
           Bytes = Constant(3);
      llvm::LoadInst *PointerOrigin = nullptr;
      if (G.FunctionPointer) {
        llvm::DominatorTree DT(*Call->getFunction());
        PointerOrigin =
            pointerCandidateLoad(Call->getArgOperand(N), Call->getArgOperand(0),
                                 DT, M.getDataLayout());
      }
      const StateSourceImport *Import = nullptr;
      if (Call->getMetadata("neverd.state.sources_ready") &&
          (G.FunctionPointer || Selector) && Offset && Bytes &&
          (*Bytes == 1 || *Bytes == 2 || *Bytes == 4 || *Bytes == 8) &&
          *Offset <= R.Contract.StateBytes &&
          *Bytes <= R.Contract.StateBytes - *Offset)
        for (const auto &I : G.Imports) {
          if (Arity && I.Parameters.size() != *Arity)
            continue;
          if (G.FunctionPointer) {
            if (!PointerOrigin)
              continue;
            const auto &P = *I.PointerSource;
            if (!pointerCandidateMatches(*PointerOrigin, Call->getArgOperand(0),
                                         R.Contract.Images[P.Image], P.Offset,
                                         M.getDataLayout()))
              continue;
          } else if (I.Selector != *Selector)
            continue;
          Import = &I;
          break;
        }
      llvm::IRBuilder<> B(Call);
      llvm::Value *ScalarMatches = B.getTrue();
      if (Import && !Arity)
        ScalarMatches = B.CreateICmpEQ(Call->getArgOperand(N + 1),
                                       B.getInt64(Import->Parameters.size()),
                                       "argument_count_matches");
      if (Import && G.FunctionPointer) {
        auto Target = M.getOrInsertFunction(Import->Name,
                                            signature(M.getContext(), *Import));
        auto *Pointer = Call->getArgOperand(N);
        ScalarMatches = B.CreateAnd(
            ScalarMatches,
            B.CreateAnd(
                B.CreateICmpNE(Pointer, B.getInt64(0)),
                B.CreateICmpEQ(Pointer, B.CreatePtrToInt(Target.getCallee(),
                                                         B.getInt64Ty()))),
            "function_pointer_matches");
      }
      for (auto E : G.ExcludedArguments)
        ScalarMatches =
            B.CreateAnd(ScalarMatches,
                        B.CreateICmpNE(Call->getArgOperand(N + 4 + E.Argument),
                                       B.getInt64(E.Value)),
                        "argument_allowed");
      struct StringCandidate {
        unsigned Argument;
        std::string Encoded;
        std::string Decoded;
      };
      std::vector<StringCandidate> Candidates;
      if (Import)
        for (unsigned I = 0; I < G.Strings.size(); ++I) {
          const auto &S = G.Strings[I];
          unsigned Extra = 4 + G.Arguments.size() + 3 * I;
          auto Start = Constant(Extra), Mode = Constant(Extra + 1),
               Key = Constant(Extra + 2);
          const auto &Image = R.Contract.Images[S.Image];
          if (!Start || *Start >= Image.Bytes.size() || !Key) {
            Import = nullptr;
            break;
          }
          uint8_t Xor = 0;
          if (S.Mode && *Key && (!Mode || *Mode == S.EncodedMode)) {
            Xor = uint8_t(*Key);
            ScalarMatches =
                B.CreateAnd(ScalarMatches,
                            B.CreateICmpEQ(Call->getArgOperand(N + Extra + 1),
                                           B.getInt64(S.EncodedMode)),
                            "string_mode_matches");
          }
          StringCandidate Candidate{S.Argument, {}, {}};
          bool Terminated = false;
          for (size_t P = *Start;
               P < Image.Bytes.size() && Candidate.Encoded.size() < 512; ++P) {
            uint8_t Byte = Image.Bytes[P];
            Candidate.Encoded.push_back(char(Byte));
            if (uint8_t(Byte ^ Xor) == 0) {
              Terminated = true;
              break;
            }
            Candidate.Decoded.push_back(char(Byte ^ Xor));
          }
          if (!Terminated) {
            Import = nullptr;
            break;
          }
          Candidates.push_back(std::move(Candidate));
        }
      if (auto *Known = llvm::dyn_cast<llvm::ConstantInt>(ScalarMatches);
          Known && Known->isZero())
        Import = nullptr;
      auto OriginalCall = [&](llvm::IRBuilder<> &B) {
        std::vector<llvm::Value *> Args;
        std::vector<llvm::Type *> Types;
        for (unsigned I = 0; I < N; ++I)
          Args.push_back(Call->getArgOperand(I));
        for (auto *A : Args)
          Types.push_back(A->getType());
        // O1 can prune the now-unused original declaration. The probe retains
        // its exact original parameter types, including an optional context.
        auto Original = M.getOrInsertFunction(
            G.Name, llvm::FunctionType::get(Call->getType(), Types, false));
        return B.CreateCall(Original, Args, "unresolved_state_call");
      };
      auto NativeCall = [&](llvm::IRBuilder<> &B) {
        std::vector<llvm::Value *> Args;
        for (size_t I = 0; I < Import->Parameters.size(); ++I) {
          auto *V = Call->getArgOperand(N + 4 + I);
          auto *T = type(M.getContext(), Import->Parameters[I]);
          Args.push_back(T->isPointerTy() ? B.CreateIntToPtr(V, T)
                                          : B.CreateZExtOrTrunc(V, T));
        }
        for (const auto &S : Candidates)
          Args[S.Argument] = B.CreateGlobalString(S.Decoded, "source_string");
        auto *Scope = Call->getMetadata("neverd.state.scope");
        // Fixed bank writes occur before the imported operation, matching the
        // declared gateway contract. Guest effects cannot observe this bank.
        for (auto W : G.Writes)
          write(B, Call->getArgOperand(0), W.Slot, B.getInt64(W.Value), Scope);
        auto Target = M.getOrInsertFunction(Import->Name,
                                            signature(M.getContext(), *Import));
        auto *Declaration = llvm::cast<llvm::Function>(Target.getCallee());
        if (signedType(Import->Result))
          Declaration->addFnAttr(LLVMSourceSignedResult);
        for (unsigned I = 0; I < Import->Parameters.size(); ++I)
          if (signedType(Import->Parameters[I]))
            Declaration->addParamAttr(
                I, llvm::Attribute::get(M.getContext(),
                                        LLVMSourceSignedParameter));
        auto *Native = B.CreateCall(Target, Args,
                                    Import->Result == StateSourceType::Void
                                        ? ""
                                        : Import->Name + "_result");
        for (const auto &S : Candidates)
          Native->addParamAttr(
              S.Argument,
              llvm::Attribute::get(M.getContext(),
                                   LLVMSourceByValueStringParameter));
        Native->setMetadata(llvm::LLVMContext::MD_noalias, Scope);
        llvm::Value *Result = B.getInt64(0);
        if (Native->getType()->isPointerTy())
          Result = B.CreatePtrToInt(Native, B.getInt64Ty());
        else if (!Native->getType()->isVoidTy())
          Result = B.CreateIntCast(Native, B.getInt64Ty(),
                                   signedType(Import->Result));
        write(B, Call->getArgOperand(0), {unsigned(*Offset), unsigned(*Bytes)},
              Result, Scope);
      };
      if (!Import) {
        Call->replaceAllUsesWith(OriginalCall(B));
        ++R.UnresolvedCalls;
      } else if (llvm::isa<llvm::ConstantInt>(ScalarMatches) &&
                 Candidates.empty()) {
        NativeCall(B);
        Call->replaceAllUsesWith(B.getInt64(0));
        ++R.RewrittenCalls;
      } else {
        auto *Entry = Call->getParent();
        auto *F = Entry->getParent();
        auto *Tail =
            Entry->splitBasicBlock(Call->getIterator(), "call_continue");
        auto *Then =
            llvm::BasicBlock::Create(M.getContext(), "source_call", F, Tail);
        auto *Else =
            llvm::BasicBlock::Create(M.getContext(), "original_call", F, Tail);
        auto *Check = Candidates.empty()
                          ? Then
                          : llvm::BasicBlock::Create(M.getContext(),
                                                     "string_check", F, Then);
        Entry->getTerminator()->eraseFromParent();
        llvm::IRBuilder<> Branch(Entry);
        Branch.CreateCondBr(ScalarMatches, Check, Else);
        if (!Candidates.empty()) {
          llvm::IRBuilder<> CheckBuilder(Check);
          llvm::Value *Matches = CheckBuilder.getTrue();
          for (const auto &S : Candidates) {
            auto *Pointer = CheckBuilder.CreateIntToPtr(
                Call->getArgOperand(N + 4 + S.Argument),
                CheckBuilder.getPtrTy());
            // The contract supplies a readable extent. Compare every candidate
            // byte including its terminator; snapshots never establish current
            // memory contents. Unaligned integer loads are endian-aware.
            for (size_t P = 0; P < S.Encoded.size();) {
              unsigned Width = 8;
              while (Width > S.Encoded.size() - P)
                Width /= 2;
              uint64_t Expected = 0;
              for (unsigned J = 0; J < Width; ++J) {
                unsigned Shift =
                    M.getDataLayout().isLittleEndian() ? J : Width - 1 - J;
                Expected |= uint64_t(uint8_t(S.Encoded[P + J])) << (Shift * 8);
              }
              auto *T = CheckBuilder.getIntNTy(Width * 8);
              auto *Load = CheckBuilder.CreateAlignedLoad(
                  T, slot(CheckBuilder, Pointer, P), llvm::Align(1),
                  "string_bytes");
              Load->setMetadata(llvm::LLVMContext::MD_noalias,
                                Call->getMetadata("neverd.state.scope"));
              Matches = CheckBuilder.CreateAnd(
                  Matches,
                  CheckBuilder.CreateICmpEQ(
                      Load, llvm::ConstantInt::get(T, Expected)),
                  "string_matches");
              P += Width;
            }
          }
          CheckBuilder.CreateCondBr(Matches, Then, Else);
        }
        llvm::IRBuilder<> Known(Then), Unknown(Else);
        NativeCall(Known);
        Known.CreateBr(Tail);
        auto *Status = OriginalCall(Unknown);
        Unknown.CreateBr(Tail);
        auto *Merged = llvm::PHINode::Create(B.getInt64Ty(), 2, "call_status",
                                             Call->getIterator());
        Merged->addIncoming(B.getInt64(0), Then);
        Merged->addIncoming(Status, Else);
        Call->replaceAllUsesWith(Merged);
        ++R.RewrittenCalls;
        ++R.GuardedCalls;
      }
      Call->eraseFromParent();
    }
    Probe->eraseFromParent();
  }
  if (llvm::verifyModule(M, &llvm::errs()))
    return invalid("source projection failed verification");
  return llvm::Error::success();
}
} // namespace neverd
