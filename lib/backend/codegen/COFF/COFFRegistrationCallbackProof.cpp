//===- COFFRegistrationCallbackProof.cpp - PE32 callback identities -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFNativeEHProvenance.h"
#include "COFFRegistrationIRProof.h"

#include "neverd/backend/llvm/WindowsRegistrationFrame.h"

#include "llvm/IR/Dominators.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"

#include <set>

#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
namespace neverd::coff_registration {
namespace {
using Role = windows_eh_md::NativeProvenanceRole;
constexpr auto Model = windows_eh_md::NativeProvenanceModel::X86RegistrationSEH;
} // namespace

llvm::Expected<std::map<CallbackKey, const llvm::Function *>>
callbacks(const llvm::Function &Parent, const ExceptionFunction &Source,
          const std::map<CallbackKey, std::set<uint8_t>> *ExpectedRoots,
          std::map<const llvm::Function *, const llvm::StoreInst *>
              *ExceptionBridges) {
  auto Frame = registrationFrame(Parent, Source);
  if (!Frame)
    return Frame.takeError();
  std::map<CallbackKey, const llvm::Function *> Result;
  std::set<uint32_t> Indices;
  const llvm::CallBase *Escape = nullptr;
  for (const auto &Block : Parent)
    for (const auto &Instruction : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
          Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape) {
        if (Escape || &Block != &Parent.getEntryBlock())
          return rejectIR(
              "parent localescape is not unique in the entry block");
        Escape = Call;
        for (const llvm::Value *Argument : Call->args()) {
          const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Argument);
          if (!Slot || !Slot->isStaticAlloca() ||
              Slot->getFunction() != &Parent ||
              Slot->getParent() != &Parent.getEntryBlock())
            return rejectIR(
                "parent localescape does not own a static frame slot");
        }
      }
  for (const llvm::Function &Function : *Parent.getParent()) {
    if (&Function == &Parent)
      continue;
    bool Found = false;
    bool Finally = false;
    va_t CallbackEntry = 0;
    for (const auto &Block : Function)
      for (const auto &Instruction : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
        if (!Call ||
            !Call->countOperandBundlesOfType(windows_eh_md::ProvenanceBundle))
          continue;
        auto P = coff_native_eh::parseNativeEHProvenance(*Call);
        if (!P || P->FunctionVA != Source.CodeRange.Begin)
          continue;
        if (P->Role != Role::RegistrationCallback || P->Model != Model ||
            P->Flags > 1 || P->Clause || P->AuxVA || Found ||
            &Function == &Parent || Call->getNumOperandBundles() != 1 ||
            !Indices.insert(P->Region).second ||
            !Result.emplace(CallbackKey{P->SourceVA, P->Flags != 0}, &Function)
                 .second)
          return rejectIR("callback occurrence is malformed or duplicated");
        Finally = P->Flags != 0;
        CallbackEntry = P->SourceVA;
        Found = true;
      }
    if (!Found)
      continue;
    auto *Ty = Function.getFunctionType();
    if (llvm::verifyFunction(Function) || !Function.hasLocalLinkage() ||
        Ty->isVarArg() || !Function.hasFnAttribute(llvm::Attribute::NoInline) ||
        !Function.hasFnAttribute(llvm::Attribute::OptimizeNone) ||
        Function.getFnAttribute("frame-pointer").getValueAsString() != "all" ||
        Function.hasPersonalityFn() ||
        (Finally
             ? (!Ty->getReturnType()->isVoidTy() || Ty->getNumParams() != 2 ||
                !Ty->getParamType(0)->isIntegerTy(8) ||
                !Ty->getParamType(1)->isPointerTy() ||
                Ty->getParamType(1)->getPointerAddressSpace() != 0 ||
                !Function.getArg(0)->hasAttribute(llvm::Attribute::ZExt))
             : (!Ty->getReturnType()->isIntegerTy(32) || Ty->getNumParams())))
      return rejectIR("callback ABI or frame ownership changed");
    llvm::DominatorTree Dominators(const_cast<llvm::Function &>(Function));
    unsigned RuntimeFrames = 0, RecoveredFrames = 0, ExceptionCells = 0;
    std::set<uint8_t> RootKinds;
    for (const auto &Block : Function)
      for (const auto &Instruction : Block) {
        if (const auto *Root = Instruction.getMetadata(
                windows_eh_md::RegistrationRootAttachment)) {
          auto Kind = metadataInteger(*Root, 0, 8);
          if (!Kind || !RootKinds.insert(*Kind).second)
            return rejectIR("callback duplicates a runtime register root");
        }
        if (const auto *Root = Instruction.getMetadata(
                windows_eh_md::RegistrationRootAttachment)) {
          const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
          const auto *Value =
              Store
                  ? llvm::dyn_cast<llvm::PtrToIntInst>(Store->getValueOperand())
                  : nullptr;
          const auto *Address = Value ? llvm::dyn_cast<llvm::GetElementPtrInst>(
                                            Value->getPointerOperand())
                                      : nullptr;
          auto Kind = metadataInteger(*Root, 0, 8);
          auto Offset = metadataInteger(*Root, 1, 64);
          llvm::APInt Actual(32, 0);
          if (Root->getNumOperands() != 2 || !Store || !Kind || *Kind > 1 ||
              !Offset || *Offset > UINT32_MAX || !Value ||
              !Value->getType()->isIntegerTy(32) || !Address ||
              !Address->accumulateConstantOffset(
                  Parent.getParent()->getDataLayout(), Actual) ||
              Actual.getZExtValue() != *Offset)
            return rejectIR(
                "callback runtime root lost its frame address recipe");
          const auto *Slot =
              llvm::dyn_cast<llvm::AllocaInst>(Store->getPointerOperand());
          if (!Slot || Slot->getFunction() != &Function ||
              !Slot->isStaticAlloca() ||
              Slot->getParent() != &Function.getEntryBlock() ||
              Slot->getAddressSpace() ||
              !Slot->getAllocatedType()->isIntegerTy(32) ||
              Slot->getAllocationSize(Parent.getParent()->getDataLayout()) !=
                  llvm::TypeSize::getFixed(4) ||
              Store->isAtomic())
            return rejectIR(
                "callback runtime root has no private scalar destination");
          unsigned Loads = 0;
          for (const auto *User : Slot->users()) {
            if (User == Store)
              continue;
            const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
            if (!Load || Load->getPointerOperand() != Slot ||
                Load->isAtomic() || !Dominators.dominates(Store, Load))
              return rejectIR(
                  "callback runtime root does not dominate its scalar reads");
            ++Loads;
          }
          if (!Loads)
            return rejectIR(
                "callback runtime root was redirected into unused storage");
          if (*Kind == 0) {
            const auto *Recover = llvm::dyn_cast<llvm::IntrinsicInst>(
                Address->getPointerOperand());
            auto Index = Recover && Recover->arg_size() == 3
                             ? integer(Recover->getArgOperand(2), 32)
                             : std::nullopt;
            if (*Offset != Frame->EntrySP - 4 || !Recover ||
                Recover->getIntrinsicID() != llvm::Intrinsic::localrecover ||
                !Index || !Escape || *Index >= Escape->arg_size() ||
                Escape->getArgOperand(*Index) != Frame->Slot)
              return rejectIR(
                  "callback EBP no longer names its logical parent frame");
          } else {
            const auto *Stack =
                llvm::dyn_cast<llvm::AllocaInst>(Address->getPointerOperand());
            auto Bytes = Stack ? Stack->getAllocationSize(
                                     Parent.getParent()->getDataLayout())
                               : std::nullopt;
            if (!Stack || Stack->getFunction() != &Function ||
                !Stack->isStaticAlloca() || !Bytes || Bytes->isScalable() ||
                !*Offset || *Offset > Bytes->getFixedValue())
              return rejectIR(
                  "callback ESP no longer names bounded private storage");
          }
        }
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction)) {
          const auto *Load =
              llvm::dyn_cast<llvm::LoadInst>(Store->getValueOperand());
          const auto *Cell =
              Load
                  ? llvm::dyn_cast<llvm::GEPOperator>(Load->getPointerOperand())
                  : nullptr;
          const auto *Runtime = Cell ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                           Cell->getPointerOperand())
                                     : nullptr;
          if (Runtime &&
              Runtime->getIntrinsicID() == llvm::Intrinsic::frameaddress) {
            const auto *Target =
                llvm::dyn_cast<llvm::GEPOperator>(Store->getPointerOperand());
            const auto *Recover = Target ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                               Target->getPointerOperand())
                                         : nullptr;
            auto Index = Recover && Recover->arg_size() == 3
                             ? integer(Recover->getArgOperand(2), 32)
                             : std::nullopt;
            llvm::APInt RuntimeOffset(32, 0), LogicalOffset(32, 0);
            if (Finally || Store->getParent() != &Function.getEntryBlock() ||
                Load->getParent() != &Function.getEntryBlock() ||
                !Load->getType()->isPointerTy() || Load->isAtomic() ||
                Store->isAtomic() || Store->getAlign() != llvm::Align(1) ||
                !Cell->accumulateConstantOffset(
                    Parent.getParent()->getDataLayout(), RuntimeOffset) ||
                RuntimeOffset.getSExtValue() != -20 || !Recover ||
                Recover->getIntrinsicID() != llvm::Intrinsic::localrecover ||
                !Index || !Escape || *Index >= Escape->arg_size() ||
                Escape->getArgOperand(*Index) != Frame->Slot ||
                !Target->accumulateConstantOffset(
                    Parent.getParent()->getDataLayout(), LogicalOffset) ||
                LogicalOffset.getZExtValue() != Frame->EntrySP - 24 ||
                ++ExceptionCells != 1)
              return rejectIR("filter exception pointers lost their exact "
                              "logical frame cell");
            if (ExceptionBridges)
              ExceptionBridges->emplace(&Function, Store);
          }
        }
        for (const auto &Operand : Instruction.operands())
          if (Operand->getType()->isPointerTy() &&
              Operand->getType()->getPointerAddressSpace() == 257)
            return rejectIR("callback has an unauthenticated FS observation");
        const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
        if (!Call)
          continue;
        switch (Call->getIntrinsicID()) {
        case llvm::Intrinsic::frameaddress:
          if (Finally || Call->arg_size() != 1 ||
              integer(Call->getArgOperand(0), 32) != 1)
            return rejectIR("filter does not recover the runtime frame");
          ++RuntimeFrames;
          break;
        case llvm::Intrinsic::eh_recoverfp: {
          const auto *Frame =
              llvm::dyn_cast<llvm::IntrinsicInst>(Call->getArgOperand(1));
          if (Finally || Call->arg_size() != 2 ||
              Call->getArgOperand(0) != &Parent || !Frame ||
              Frame->getIntrinsicID() != llvm::Intrinsic::frameaddress)
            return rejectIR("filter recovery names a different parent frame");
          ++RecoveredFrames;
          break;
        }
        case llvm::Intrinsic::localrecover: {
          auto Index = integer(Call->getArgOperand(2), 32);
          if (Call->arg_size() != 3 || Call->getArgOperand(0) != &Parent ||
              !Escape || !Index || *Index >= Escape->arg_size())
            return rejectIR(
                "callback localrecover lost its exact escape index");
          const llvm::Value *Frame = Call->getArgOperand(1);
          const auto *Recover = llvm::dyn_cast<llvm::IntrinsicInst>(Frame);
          if (Finally ? Frame != Function.getArg(1)
                      : (!Recover || Recover->getIntrinsicID() !=
                                         llvm::Intrinsic::eh_recoverfp))
            return rejectIR(
                "callback localrecover uses the wrong runtime frame");
          break;
        }
        case llvm::Intrinsic::localescape:
        case llvm::Intrinsic::localaddress:
        case llvm::Intrinsic::seh_scope_begin:
        case llvm::Intrinsic::seh_scope_end:
          return rejectIR(
              "callback creates a second registration-frame protocol");
        default:
          break;
        }
      }
    if (Finally
            ? RuntimeFrames || RecoveredFrames || ExceptionCells
            : RuntimeFrames != 1 || RecoveredFrames != 1 || ExceptionCells != 1)
      return rejectIR("callback runtime frame recovery set is incomplete");
    if (ExpectedRoots) {
      auto Expected = ExpectedRoots->find({CallbackEntry, Finally});
      if (Expected == ExpectedRoots->end() || Expected->second != RootKinds)
        return rejectIR(
            "callback runtime root set differs from immutable source live-ins");
    }
  }
  std::set<CallbackKey> Expected;
  for (const auto &Scope : Source.Registration->Scopes)
    Expected.emplace(Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA,
                     Scope.IsFinally);
  if (Result.size() != Expected.size())
    return rejectIR("callback set is not closed over the source scope table");
  for (const auto &Key : Expected)
    if (!Result.count(Key))
      return rejectIR("source callback has no generated frame owner");
  return Result;
}

} // namespace neverd::coff_registration
#endif
