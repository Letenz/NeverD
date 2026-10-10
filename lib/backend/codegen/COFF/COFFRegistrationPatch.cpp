//===- COFFRegistrationPatch.cpp - Checked PE32 SEH installation ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"

#include "COFFNativeEHProvenance.h"
#include "COFFRegistrationCxxIRProof.h"
#include "COFFRegistrationFrameProof.h"
#include "COFFRegistrationIRProof.h"
#include "COFFRegistrationTableProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/object/PELayout.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/SHA256.h"
#include "llvm/TargetParser/Triple.h"

#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {

llvm::Error reject(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "coff registration patch: " + Detail);
}

#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
using Role = windows_eh_md::NativeProvenanceRole;
constexpr auto Model = windows_eh_md::NativeProvenanceModel::X86RegistrationSEH;
using Provenance = coff_native_eh::NativeEHProvenance;

using coff_registration::CallbackKey;
using coff_registration::exactSemanticToken;
using coff_registration::integer;
using coff_registration::matchesSourceOperation;
using coff_registration::metadataInteger;
using coff_registration::next;
using coff_registration::RegistrationFrame;
using coff_registration::registrationFrame;
using coff_registration::validateSourceSegments;
bool frameAddress(const llvm::Value *Address, const RegistrationFrame &Frame,
                  uint64_t Offset) {
  const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Address);
  llvm::APInt Actual(32, 0);
  return GEP && GEP->getPointerOperand() == Frame.Slot &&
         GEP->getPointerAddressSpace() == 0 &&
         GEP->accumulateConstantOffset(Frame.Slot->getModule()->getDataLayout(),
                                       Actual) &&
         Actual.getZExtValue() == Offset;
}

llvm::Expected<std::map<CallbackKey, const llvm::Function *>> callbacks(
    const llvm::Function &Parent, const ExceptionFunction &Source,
    const std::map<CallbackKey, std::set<uint8_t>> *ExpectedRoots = nullptr,
    std::map<const llvm::Function *, const llvm::StoreInst *>
        *ExceptionBridges = nullptr) {
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
          return reject("parent localescape is not unique in the entry block");
        Escape = Call;
        for (const llvm::Value *Argument : Call->args()) {
          const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Argument);
          if (!Slot || !Slot->isStaticAlloca() ||
              Slot->getFunction() != &Parent ||
              Slot->getParent() != &Parent.getEntryBlock())
            return reject(
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
          return reject("callback occurrence is malformed or duplicated");
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
      return reject("callback ABI or frame ownership changed");
    llvm::DominatorTree Dominators(const_cast<llvm::Function &>(Function));
    unsigned RuntimeFrames = 0, RecoveredFrames = 0, ExceptionCells = 0;
    std::set<uint8_t> RootKinds;
    for (const auto &Block : Function)
      for (const auto &Instruction : Block) {
        if (const auto *Root = Instruction.getMetadata(
                windows_eh_md::RegistrationRootAttachment)) {
          auto Kind = metadataInteger(*Root, 0, 8);
          if (!Kind || !RootKinds.insert(*Kind).second)
            return reject("callback duplicates a runtime register root");
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
            return reject(
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
            return reject(
                "callback runtime root has no private scalar destination");
          unsigned Loads = 0;
          for (const auto *User : Slot->users()) {
            if (User == Store)
              continue;
            const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
            if (!Load || Load->getPointerOperand() != Slot ||
                Load->isAtomic() || !Dominators.dominates(Store, Load))
              return reject(
                  "callback runtime root does not dominate its scalar reads");
            ++Loads;
          }
          if (!Loads)
            return reject(
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
              return reject(
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
              return reject(
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
              return reject("filter exception pointers lost their exact "
                            "logical frame cell");
            if (ExceptionBridges)
              ExceptionBridges->emplace(&Function, Store);
          }
        }
        for (const auto &Operand : Instruction.operands())
          if (Operand->getType()->isPointerTy() &&
              Operand->getType()->getPointerAddressSpace() == 257)
            return reject("callback has an unauthenticated FS observation");
        const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
        if (!Call)
          continue;
        switch (Call->getIntrinsicID()) {
        case llvm::Intrinsic::frameaddress:
          if (Finally || Call->arg_size() != 1 ||
              integer(Call->getArgOperand(0), 32) != 1)
            return reject("filter does not recover the runtime frame");
          ++RuntimeFrames;
          break;
        case llvm::Intrinsic::eh_recoverfp: {
          const auto *Frame =
              llvm::dyn_cast<llvm::IntrinsicInst>(Call->getArgOperand(1));
          if (Finally || Call->arg_size() != 2 ||
              Call->getArgOperand(0) != &Parent || !Frame ||
              Frame->getIntrinsicID() != llvm::Intrinsic::frameaddress)
            return reject("filter recovery names a different parent frame");
          ++RecoveredFrames;
          break;
        }
        case llvm::Intrinsic::localrecover: {
          auto Index = integer(Call->getArgOperand(2), 32);
          if (Call->arg_size() != 3 || Call->getArgOperand(0) != &Parent ||
              !Escape || !Index || *Index >= Escape->arg_size())
            return reject("callback localrecover lost its exact escape index");
          const llvm::Value *Frame = Call->getArgOperand(1);
          const auto *Recover = llvm::dyn_cast<llvm::IntrinsicInst>(Frame);
          if (Finally ? Frame != Function.getArg(1)
                      : (!Recover || Recover->getIntrinsicID() !=
                                         llvm::Intrinsic::eh_recoverfp))
            return reject("callback localrecover uses the wrong runtime frame");
          break;
        }
        case llvm::Intrinsic::localescape:
        case llvm::Intrinsic::localaddress:
        case llvm::Intrinsic::seh_scope_begin:
        case llvm::Intrinsic::seh_scope_end:
          return reject(
              "callback creates a second registration-frame protocol");
        default:
          break;
        }
      }
    if (Finally
            ? RuntimeFrames || RecoveredFrames || ExceptionCells
            : RuntimeFrames != 1 || RecoveredFrames != 1 || ExceptionCells != 1)
      return reject("callback runtime frame recovery set is incomplete");
    if (ExpectedRoots) {
      auto Expected = ExpectedRoots->find({CallbackEntry, Finally});
      if (Expected == ExpectedRoots->end() || Expected->second != RootKinds)
        return reject(
            "callback runtime root set differs from immutable source live-ins");
    }
  }
  std::set<CallbackKey> Expected;
  for (const auto &Scope : Source.Registration->Scopes)
    Expected.emplace(Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA,
                     Scope.IsFinally);
  if (Result.size() != Expected.size())
    return reject("callback set is not closed over the source scope table");
  for (const auto &Key : Expected)
    if (!Result.count(Key))
      return reject("source callback has no generated frame owner");
  return Result;
}

llvm::Expected<size_t> rawOffset(const PEHeaderPtrs &PE,
                                 llvm::ArrayRef<uint8_t> Bytes, uint32_t RVA,
                                 uint32_t Size) {
  std::vector<coff_loader::detail::RawBackedSectionRange> Sections;
  forEachPESection(PE, [&](const PESectionFields &S, uint16_t) {
    Sections.push_back({S.VirtualAddress,
                        getPESectionContentSize(S.VirtualSize, S.SizeOfRawData),
                        S.PointerToRawData, S.SizeOfRawData});
  });
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      Sections, Bytes.size(), RVA, Size);
  if (!Offset)
    return Offset.takeError();
  return static_cast<size_t>(*Offset);
}

using coff_registration::absolutePointer;
using coff_registration::exactPointerFixup;
using coff_registration::ownerVA;
using coff_registration::sectionAt;

llvm::Error validateSymbolicImagePointers(const llvm::Module &Module,
                                          uint64_t ImageBase,
                                          uint64_t ImageSize) {
  std::vector<const llvm::Value *> Pending, PointerIntegers;
  std::map<const llvm::Value *, std::vector<const llvm::Value *>> StoredValues;
  for (const auto &Global : Module.globals())
    if (Global.hasInitializer())
      Pending.push_back(Global.getInitializer());
  for (const auto &Function : Module)
    for (const auto &Block : Function)
      for (const auto &I : Block) {
        Pending.push_back(&I);
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
          StoredValues[llvm::getUnderlyingObject(Store->getPointerOperand())]
              .push_back(Store->getValueOperand());
      }
  std::set<const llvm::Value *> Seen;
  size_t Work = 0;
  while (!Pending.empty()) {
    const auto *Value = Pending.back();
    Pending.pop_back();
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return reject("symbolic pointer audit exceeds the work budget");
    if (!Seen.insert(Value).second || llvm::isa<llvm::GlobalValue>(Value))
      continue;
    const auto *User = llvm::dyn_cast<llvm::User>(Value);
    if (!User)
      continue;
    unsigned Opcode = 0;
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(Value))
      Opcode = I->getOpcode();
    if (const auto *Expression = llvm::dyn_cast<llvm::ConstantExpr>(Value))
      Opcode = Expression->getOpcode();
    if (Opcode == llvm::Instruction::IntToPtr)
      PointerIntegers.push_back(User->getOperand(0));
    for (const auto &Operand : User->operands())
      Pending.push_back(Operand.get());
  }
  // A private scalar spill does not turn an absolute image address into a
  // relocatable value. Audit the integer def-use graph and every possible
  // store to a loaded local/global object, rather than only direct constants.
  using Constants = std::set<llvm::ConstantInt *>;
  std::map<const llvm::Value *, Constants> NumericValues;
  std::set<const llvm::Value *> Evaluating;
  bool NumericBudgetExceeded = false;
  bool NumericChanged = false;
  bool IncompleteNumericMemory = false;
  std::function<Constants(const llvm::Value *, unsigned)> Evaluate;
  Evaluate = [&](const llvm::Value *Value, unsigned Depth) -> Constants {
    if (NumericBudgetExceeded || ++Work > limits::kMaxRegistrationEHStateWork ||
        Depth > 128) {
      NumericBudgetExceeded = true;
      return {};
    }
    if (!Value->getType()->isIntegerTy())
      return {};
    if (!Evaluating.insert(Value).second)
      return NumericValues[Value];
    Constants Values;
    auto Add = [&](const Constants &Other) {
      Values.insert(Other.begin(), Other.end());
      if (Values.size() > 32)
        NumericBudgetExceeded = true;
    };
    if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(Value)) {
      Values.insert(const_cast<llvm::ConstantInt *>(C));
    } else if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
      llvm::SmallVector<const llvm::Value *, 8> Objects;
      llvm::getUnderlyingObjects(Load->getPointerOperand(), Objects);
      for (const auto *Object : Objects) {
        if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Object);
            Global && Global->hasInitializer()) {
          if (Global->getInitializer()->getType() != Value->getType())
            IncompleteNumericMemory = true;
          else
            Add(Evaluate(Global->getInitializer(), Depth + 1));
        }
        if (auto Stores = StoredValues.find(Object);
            Stores != StoredValues.end()) {
          for (const auto *Stored : Stores->second) {
            if (Stored->getType() != Value->getType())
              IncompleteNumericMemory = true;
            else
              Add(Evaluate(Stored, Depth + 1));
          }
        }
      }
    } else if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(Value)) {
      for (const auto &Incoming : Phi->incoming_values())
        Add(Evaluate(Incoming.get(), Depth + 1));
    } else if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Value)) {
      Add(Evaluate(Select->getTrueValue(), Depth + 1));
      Add(Evaluate(Select->getFalseValue(), Depth + 1));
    } else if (const auto *Argument = llvm::dyn_cast<llvm::Argument>(Value)) {
      for (const auto *User : Argument->getParent()->users()) {
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
            Call &&
            Call->getCalledOperand()->stripPointerCasts() ==
                Argument->getParent() &&
            Argument->getArgNo() < Call->arg_size())
          Add(Evaluate(Call->getArgOperand(Argument->getArgNo()), Depth + 1));
        else if (Argument->getParent()->hasLocalLinkage())
          IncompleteNumericMemory = true;
      }
    } else if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(Value)) {
      llvm::SmallVector<const llvm::Value *, 8> Targets;
      llvm::getUnderlyingObjects(Call->getCalledOperand(), Targets);
      for (const auto *Target : Targets) {
        const auto *Callee = llvm::dyn_cast<llvm::Function>(Target);
        if (!Callee) {
          IncompleteNumericMemory = true;
          continue;
        }
        if (!Callee->isDeclaration())
          for (const auto &Block : *Callee)
            if (const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                Return && Return->getReturnValue())
              Add(Evaluate(Return->getReturnValue(), Depth + 1));
      }
    } else if (const auto *Binary =
                   llvm::dyn_cast<llvm::BinaryOperator>(Value)) {
      auto Left = Evaluate(Binary->getOperand(0), Depth + 1);
      auto Right = Evaluate(Binary->getOperand(1), Depth + 1);
      for (auto *L : Left)
        for (auto *R : Right) {
          if (++Work > limits::kMaxRegistrationEHStateWork) {
            NumericBudgetExceeded = true;
            break;
          }
          if (auto *C = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                  llvm::ConstantFoldBinaryOpOperands(Binary->getOpcode(), L, R,
                                                     Module.getDataLayout())))
            Add({C});
        }
    } else if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value);
               Cast && Cast->getSrcTy()->isIntegerTy()) {
      for (auto *Operand : Evaluate(Cast->getOperand(0), Depth + 1))
        if (auto *C = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                llvm::ConstantFoldCastOperand(Cast->getOpcode(), Operand,
                                              Cast->getDestTy(),
                                              Module.getDataLayout())))
          Add({C});
    }
    Evaluating.erase(Value);
    auto &Known = NumericValues[Value];
    const size_t PreviousSize = Known.size();
    Known.insert(Values.begin(), Values.end());
    NumericChanged |= Known.size() != PreviousSize;
    NumericBudgetExceeded |= Known.size() > 32;
    return Known;
  };
  Seen.clear();
  while (!PointerIntegers.empty()) {
    const auto *Value = PointerIntegers.back();
    PointerIntegers.pop_back();
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return reject("symbolic pointer origin audit exceeds the work budget");
    if (!Seen.insert(Value).second)
      continue;
    // Cyclic PHIs and memory spills need a monotone closure. An empty result
    // during recursive evaluation is not evidence that the cycle is symbolic.
    do {
      NumericChanged = false;
      for (const auto *Numeric : Evaluate(Value, 0)) {
        const uint64_t Address =
            Numeric->getValue().zextOrTrunc(32).getZExtValue();
        if (Address >= ImageBase && Address - ImageBase < ImageSize)
          return reject(
              "raw image pointer has no symbolic HIGHLOW relocation owner");
      }
    } while (NumericChanged && !NumericBudgetExceeded);
    if (IncompleteNumericMemory)
      return reject(
          "raw image pointer origin has an unproved partial memory definition");
    if (NumericBudgetExceeded)
      return reject("symbolic pointer constant audit exceeds the work budget");
    if (llvm::isa<llvm::ConstantInt>(Value)) {
      continue;
    }
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(Value))
      if (const auto *Folded =
              llvm::ConstantFoldInstruction(I, Module.getDataLayout());
          Folded && Folded != Value)
        PointerIntegers.push_back(Folded);
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
      llvm::SmallVector<const llvm::Value *, 8> Objects;
      llvm::getUnderlyingObjects(Load->getPointerOperand(), Objects);
      for (const auto *Object : Objects) {
        if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Object);
            Global && Global->hasInitializer())
          PointerIntegers.push_back(Global->getInitializer());
        if (auto Stores = StoredValues.find(Object);
            Stores != StoredValues.end())
          PointerIntegers.insert(PointerIntegers.end(), Stores->second.begin(),
                                 Stores->second.end());
      }
    }
    // Calls produce runtime values. Symbolic globals already carry MC fixups;
    // neither is a numeric address leaf in the generated program.
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value);
        Global && Global->hasInitializer())
      PointerIntegers.push_back(Global->getInitializer());
    if (llvm::isa<llvm::GlobalValue>(Value) || llvm::isa<llvm::CallBase>(Value))
      continue;
    if (const auto *User = llvm::dyn_cast<llvm::User>(Value))
      for (const auto &Operand : User->operands())
        PointerIntegers.push_back(Operand.get());
  }
  return llvm::Error::success();
}
llvm::Error
validateIncomingCallerFrame(const llvm::Function &Parent, const MedFunc &Source,
                            llvm::ArrayRef<const llvm::Function *> Functions,
                            std::set<const llvm::Instruction *> &Accesses,
                            std::set<const llvm::Instruction *> &Setup) {
  if (!Source.RegistrationStates ||
      !Source.RegistrationStates->IncomingFrameAccessesComplete)
    return reject("incoming caller frame projection is not exact");
  using Key = std::pair<va_t, int>;
  std::map<Key, const RegistrationIncomingFrameAccess *> Expected;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE)
        if (const auto *Access = Source.RegistrationStates->incomingFrameAccess(
                Op.Addr, Op.OriginSeq))
          if (!Expected.emplace(Key{Access->Address, Access->OpSeq}, Access)
                   .second)
            return reject(
                "incoming caller frame has duplicate source identities");
  const llvm::AllocaInst *Slot = nullptr;
  const llvm::IntrinsicInst *Escape = nullptr;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (const auto *MD = I.getMetadata(
                windows_eh_md::RegistrationCallerFrameAttachment)) {
          auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
          if (Slot || Function != &Parent ||
              &Block != &Parent.getEntryBlock() || !Alloca ||
              !Alloca->isStaticAlloca() ||
              !Alloca->getAllocatedType()->isPointerTy() ||
              Alloca->getAllocatedType()->getPointerAddressSpace() ||
              Alloca->getAddressSpace() || MD->getNumOperands() != 1 ||
              metadataInteger(*MD, 0, 64) != Source.Entry)
            return reject("incoming caller frame slot identity changed");
          Slot = Alloca;
        }
        if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
            Function == &Parent && Call &&
            Call->getIntrinsicID() == llvm::Intrinsic::localescape)
          Escape = Call;
      }
  if (Expected.empty() != (Slot == nullptr))
    return reject("incoming caller frame is missing its checked recovery slot");
  auto IsSlot = [&](const llvm::Value *Value) {
    if (Value == Slot && Slot)
      return true;
    const auto *Recover = llvm::dyn_cast<llvm::IntrinsicInst>(Value);
    const auto Index = Recover && Recover->arg_size() == 3
                           ? integer(Recover->getArgOperand(2), 32)
                           : std::nullopt;
    return Slot && Recover &&
           Recover->getIntrinsicID() == llvm::Intrinsic::localrecover &&
           Recover->getArgOperand(0) == &Parent && Escape && Index &&
           *Index < Escape->arg_size() && Escape->getArgOperand(*Index) == Slot;
  };
  const llvm::StoreInst *Initializer = nullptr;
  if (Slot) {
    for (const auto *Function : Functions)
      for (const auto &Block : *Function)
        for (const auto &I : Block)
          if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
              Store && IsSlot(Store->getPointerOperand())) {
            const auto *Frame =
                llvm::dyn_cast<llvm::IntrinsicInst>(Store->getValueOperand());
            if (Initializer || Function != &Parent ||
                &Block != &Parent.getEntryBlock() ||
                Store->getPointerOperand() != Slot || Store->isAtomic() ||
                !Frame || Frame->getParent() != &Block ||
                Frame->getIntrinsicID() != llvm::Intrinsic::frameaddress ||
                Frame->arg_size() != 1 ||
                integer(Frame->getArgOperand(0), 32) != 0)
              return reject(
                  "incoming caller frame lost its entry frame address");
            Initializer = Store;
            Setup.insert(Frame);
            Setup.insert(Store);
          }
    if (!Initializer)
      return reject(
          "incoming caller frame is not initialized at machine entry");
    // No hidden alias may overwrite the runtime parent-frame pointer.
    for (const auto *Function : Functions)
      for (const auto &Block : *Function)
        for (const auto &I : Block)
          if (IsSlot(&I))
            for (const auto *User : I.users()) {
              if (User == Initializer || User == Escape)
                continue;
              const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
              if (!Load || Load->getPointerOperand() != &I ||
                  Load->isAtomic() || !Load->getType()->isPointerTy())
                return reject(
                    "incoming caller frame slot escapes its recovery protocol");
            }
  }
  std::set<Key> Seen;
  llvm::DominatorTree Dominators(*const_cast<llvm::Function *>(&Parent));
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationIncomingFrameAttachment);
        if (!MD)
          continue;
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
        auto Address = metadataInteger(*MD, 1, 64),
             Seq = metadataInteger(*MD, 2, 32);
        auto Offset = metadataInteger(*MD, 3, 32),
             Width = metadataInteger(*MD, 4, 16);
        auto Write = metadataInteger(*MD, 5, 1);
        if ((!Load && !Store) || MD->getNumOperands() != 6 ||
            metadataInteger(*MD, 0, 64) != Source.Entry || !Address || !Seq ||
            *Seq > INT_MAX || !Offset || *Offset < 4 || !Width || !*Width ||
            !Write)
          return reject(
              "incoming caller frame access has a malformed identity");
        const Key Identity{*Address, int(*Seq)};
        const auto *Operation =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!Operation || Operation->getNumOperands() != 5 ||
            metadataInteger(*Operation, 0, 64) != Source.Entry ||
            metadataInteger(*Operation, 1, 64) != Address ||
            metadataInteger(*Operation, 2, 32) != Seq ||
            metadataInteger(*Operation, 4, 8) != uint8_t(bool(Store)))
          return reject("incoming caller frame access changed its source "
                        "execution occurrence");
        auto ExpectedAccess = Expected.find(Identity);
        if (ExpectedAccess == Expected.end() || !Seen.insert(Identity).second ||
            ExpectedAccess->second->Offset != *Offset ||
            ExpectedAccess->second->Width != *Width ||
            ExpectedAccess->second->Write != bool(Store) ||
            *Write != bool(Store) ||
            (Load ? Load->isAtomic() || !Load->isVolatile() ||
                        Load->getAlign() != llvm::Align(1)
                  : Store->isAtomic() || !Store->isVolatile() ||
                        Store->getAlign() != llvm::Align(1)))
          return reject("incoming caller frame access differs from checked "
                        "source memory");
        auto Size = Parent.getParent()->getDataLayout().getTypeStoreSize(
            Load ? Load->getType() : Store->getValueOperand()->getType());
        const auto *Pointer =
            Load ? Load->getPointerOperand() : Store->getPointerOperand();
        const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Pointer);
        const auto *Base =
            GEP ? llvm::dyn_cast<llvm::LoadInst>(GEP->getPointerOperand())
                : nullptr;
        auto Displacement = registration_frame::checkedByteGEPOffset(GEP);
        if (Size.isScalable() || Size.getFixedValue() != *Width || !GEP ||
            GEP->isInBounds() || GEP->hasNoUnsignedSignedWrap() ||
            !Displacement || *Displacement != int64_t(*Offset) || !Base ||
            !IsSlot(Base->getPointerOperand()) || Base->isAtomic() ||
            (Function == &Parent && !Dominators.dominates(Initializer, Base)))
          return reject("incoming caller frame access changed its physical "
                        "stack projection");
        Accesses.insert(&I);
      }
  if (Seen.size() != Expected.size())
    return reject("incoming caller frame access set is incomplete");
  return llvm::Error::success();
}
#endif
} // namespace

std::optional<va_t> findCOFFRegistrationRuntimeVA(const BinaryImage &Image,
                                                  llvm::StringRef Symbol) {
  const bool CookieSymbol = Symbol == "___security_cookie";
  if (Image.Arch != Arch::X86 || Image.Format != BinaryFormat::COFF ||
      Image.Base > UINT32_MAX ||
      (!CookieSymbol && Symbol != "@__security_check_cookie@4") ||
      !Image.DynInfo.SecurityCookieRVA)
    return std::nullopt;
  const va_t Cookie = Image.Base + Image.DynInfo.SecurityCookieRVA;
  if (Cookie > UINT32_MAX - 3 || !Image.readVA(Cookie, 4))
    return std::nullopt;
  std::optional<va_t> Result;
  for (const auto &EH : Image.ExceptionMetadata.Functions)
    if (EH.Personality == ExceptionPersonality::ExceptHandler4 &&
        EH.ParseStatus == ExceptionParseStatus::Complete && EH.Registration)
      if (auto Check = coff_loader::getCheckedX86EH4CookieCheck(
              Image, EH.PersonalityVA)) {
        if (!CookieSymbol &&
            !coff_loader::hasCheckedX86CookieCheckSuccessPath(Image, *Check))
          return std::nullopt;
        const va_t Address = CookieSymbol ? Cookie : *Check;
        if (Result && Result != Address)
          return std::nullopt;
        Result = Address;
      }
  return Result;
}

llvm::Error validateCOFFRegistrationIR(const llvm::Function &Function,
                                       const ExceptionFunction &Source,
                                       const BinaryImage &Image) {
  if (Source.Cxx)
    return validateCOFFRegistrationCxxIR(Function, Source, Image);
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return reject("LLVM does not provide indexed x86 SEH output receipts");
#else
  if (Image.Arch != Arch::X86 || Image.Format != BinaryFormat::COFF ||
      !classifyWindowsEHNativeSource(Source, Arch::X86, BinaryFormat::COFF,
                                     WindowsEHNativeCapability::IRLowering)
           .canLowerNativeIR())
    return reject("source is not a checked x86 registration SEH function");
  if (Source.Personality == ExceptionPersonality::ExceptHandler3 &&
      !coff_loader::isCheckedX86SEH3Personality(Image, Source.PersonalityVA))
    return reject("SEH3 personality has no checked CRT import forwarding path");
  const auto *Marker = Function.getMetadata(windows_eh_md::NativeAttachment);
  const auto *Version = Marker && Marker->getNumOperands() == 2
                            ? llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
                                  Marker->getOperand(0).get())
                            : nullptr;
  const auto *Kind =
      Marker && Marker->getNumOperands() == 2
          ? llvm::dyn_cast_or_null<llvm::MDString>(Marker->getOperand(1).get())
          : nullptr;
  const llvm::StringRef Name =
      Source.Personality == ExceptionPersonality::ExceptHandler4
          ? "_except_handler4"
          : "_except_handler3";
  const auto *Personality =
      Function.hasPersonalityFn()
          ? llvm::dyn_cast<llvm::Function>(Function.getPersonalityFn())
          : nullptr;
  if (!Version || integer(Version->getValue(), 1) != 1 || !Kind ||
      Kind->getString() != "seh-x86-registration-native" || !Personality ||
      Personality->getName() != Name || !Personality->isDeclaration() ||
      !Personality->hasExternalLinkage() ||
      !Personality->getReturnType()->isIntegerTy(32) ||
      !Personality->isVarArg() || Personality->arg_size() ||
      !Function.hasFnAttribute(llvm::Attribute::NoInline) ||
      !Function.hasFnAttribute(llvm::Attribute::OptimizeNone) ||
      !Function.hasFnAttribute("llvm.rewrite.win-x86-registration-state") ||
      Function.getFnAttribute("frame-pointer").getValueAsString() != "all" ||
      llvm::verifyFunction(Function))
    return reject("native x86 SEH compiler contract changed");
  auto Identity = rewrite_source::getOriginalVA(Function);
  if (!Identity)
    return Identity.takeError();
  if (!*Identity || **Identity != Source.CodeRange.Begin)
    return reject("native function has no exact source entry identity");
  auto Frame = registrationFrame(Function, Source);
  if (!Frame)
    return Frame.takeError();
  const auto &Triple = Function.getParent()->getTargetTriple();
  if (Triple.getArch() != llvm::Triple::x86 || !Triple.isOSWindows() ||
      !Triple.isOSBinFormatCOFF() || !Triple.isWindowsMSVCEnvironment() ||
      Function.getParent()->getDataLayout().getPointerSize() != 4 ||
      Function.getParent()->getModuleFlag("eh-asynch") == nullptr ||
      integer(llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                  Function.getParent()->getModuleFlag("eh-asynch")),
              32) != 1 ||
      (Source.Personality == ExceptionPersonality::ExceptHandler4 &&
       Source.Registration->GSCookieOffset != -2 &&
       !Function.hasFnAttribute(llvm::Attribute::StackProtectReq)))
    return reject(
        "native x86 SEH lost its target or asynchronous protection contract");
  Decoder Decoder;
  if (!Decoder.init(Image))
    return reject("cannot replay source registration-state analysis");
  CFGBuilder Builder;
  LowFunc Replay = Builder.build(Image, Decoder, Source.CodeRange.Begin,
                                 Function.getName().str());
  std::vector<ExceptionAddressRange> SourceCallerPCWrites;
  if (Function.getCallingConv() != llvm::CallingConv::C ||
      !hasCallerCleanupRegistrationABI(Replay, Image, &SourceCallerPCWrites))
    return reject(
        "source or preserved callee has an unproved x86 stack cleanup ABI");
  for (const auto &Block : Function)
    for (const auto &I : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
          Call && Call->getCallingConv() != llvm::CallingConv::C)
        return reject(
            "generated call changes the checked x86 stack cleanup ABI");
  if (!Replay.RegistrationStates || !Replay.RegistrationStates->Complete ||
      !Replay.RegistrationStates->CallbackStatesComplete ||
      !Replay.RegistrationStates->RegistrationLifetimeComplete ||
      !Replay.RegistrationStates->ChainOperationsComplete)
    return reject(
        "source registration lifetime or dispatch state is incomplete");
  if (Source.Personality == ExceptionPersonality::ExceptHandler4) {
    const auto &States = *Replay.RegistrationStates;
    const auto *Cookie =
        Function.getParent()->getNamedGlobal("__security_cookie");
    if (!States.SecurityCookiesComplete || !States.SecurityCookieVA ||
        States.SecurityCookieVA !=
            Image.Base + Image.DynInfo.SecurityCookieRVA ||
        !coff_loader::getCheckedX86EH4CookieCheck(Image,
                                                  Source.PersonalityVA) ||
        !Cookie || !Cookie->getValueType()->isIntegerTy(32) ||
        !Cookie->isDeclaration() || !Cookie->hasExternalLinkage() ||
        Cookie->isConstant() || Cookie->isThreadLocal() ||
        Cookie->getAddressSpace() || Cookie->hasDLLImportStorageClass())
      return reject(
          "EH4 cookie frame, image storage or CRT wrapper is unproved");
    if (Source.Registration->GSCookieOffset != -2) {
      const auto *Check =
          Function.getParent()->getFunction("__security_check_cookie");
      if (!Check || !hasX86RegistrationSecurityCheckABI(*Check))
        return reject("EH4 GS cookie checker has an incompatible runtime ABI");
      auto VA = rewrite_source::getOriginalVA(*Check);
      if (!VA)
        return VA.takeError();
      if (!*VA || **VA != *coff_loader::getCheckedX86EH4CookieCheck(
                              Image, Source.PersonalityVA))
        return reject("EH4 GS cookie checker changed its original identity");
      if (!coff_loader::hasCheckedX86CookieCheckSuccessPath(Image, **VA))
        return reject("EH4 GS cookie checker has no checked success path");
    }
  }
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  MedFunc ReplayMed = Converter.convert(Replay, Arch::X86, BinaryFormat::COFF);
  std::map<CallbackKey, std::set<uint8_t>> ExpectedRoots;
  for (const auto &Scope : Source.Registration->Scopes) {
    const CallbackKey Key{Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA,
                          Scope.IsFinally};
    auto &Roots = ExpectedRoots[Key];
    for (const auto &Block : ReplayMed.Blocks)
      if (Block.StartAddr == Key.first)
        for (const auto &Op : Block.Ops)
          if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
            Roots.insert(
                Op.RegistrationRoot ==
                        MedOp::RegistrationRootKind::EstablishedFramePointer
                    ? 0
                    : 1);
  }
  std::map<const llvm::Function *, const llvm::StoreInst *> ExceptionBridges;
  auto CallbackMap =
      callbacks(Function, Source, &ExpectedRoots, &ExceptionBridges);
  if (!CallbackMap)
    return CallbackMap.takeError();
  std::map<va_t, va_t> NormalFinallyCalls;
  for (const auto &Block : Replay.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst())
        for (const auto &Scope : Source.Registration->Scopes)
          if (Scope.IsFinally && Scope.HandlerVA == Op.Inputs[0].Offset)
            NormalFinallyCalls.emplace(Op.Addr, Scope.HandlerVA);
  std::set<va_t> SeenNormalFinallyCalls;
  std::set<const llvm::CallBase *> NormalFinallyIR;
  for (const auto &Block : Function)
    for (const auto &I : Block) {
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
      const auto *Marker =
          I.getMetadata(windows_eh_md::RegistrationFinallyCallAttachment);
      if (!Marker)
        continue;
      if (!Call || Marker->getNumOperands() != 3)
        return reject("ordinary finally call has a malformed source identity");
      auto Field = [&](unsigned Index) {
        return integer(llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                           Marker->getOperand(Index)),
                       64);
      };
      const auto Owner = Field(0), Address = Field(1), Target = Field(2);
      const auto *ParentFrame =
          Call->arg_size() == 2
              ? llvm::dyn_cast<llvm::IntrinsicInst>(Call->getArgOperand(1))
              : nullptr;
      if (Owner != Source.CodeRange.Begin || !Address || !Target ||
          !matchesSourceOperation(I, Source.CodeRange.Begin, *Address, 2) ||
          !NormalFinallyCalls.count(*Address) ||
          NormalFinallyCalls.at(*Address) != *Target ||
          !SeenNormalFinallyCalls.insert(*Address).second ||
          !CallbackMap->count({*Target, true}) ||
          Call->getCalledFunction() != CallbackMap->at({*Target, true}) ||
          Call->arg_size() != 2 || integer(Call->getArgOperand(0), 8) != 0 ||
          !ParentFrame ||
          ParentFrame->getIntrinsicID() != llvm::Intrinsic::localaddress ||
          ParentFrame->arg_size() || ParentFrame->getFunction() != &Function ||
          Call->getNumOperandBundles())
        return reject(
            "ordinary finally call changed its normal parent-frame ABI");
      NormalFinallyIR.insert(Call);
    }
  if (SeenNormalFinallyCalls.size() != NormalFinallyCalls.size())
    return reject("source ordinary finally call has no recovered callback");
  std::vector<const llvm::Function *> CallbackFunctions;
  for (const auto &[Key, Callback] : *CallbackMap)
    CallbackFunctions.push_back(Callback);
  std::vector<const llvm::Function *> NativeFunctions{&Function};
  NativeFunctions.insert(NativeFunctions.end(), CallbackFunctions.begin(),
                         CallbackFunctions.end());
  std::set<const llvm::Instruction *> IncomingAccesses, IncomingSetup;
  if (llvm::Error Error =
          validateIncomingCallerFrame(Function, ReplayMed, NativeFunctions,
                                      IncomingAccesses, IncomingSetup))
    return Error;
  const auto &States = *Replay.RegistrationStates;
  const auto &Scopes = Source.Registration->Scopes;
  struct Dispatch {
    const llvm::BasicBlock *Unwind = nullptr;
    const llvm::CatchSwitchInst *Switch = nullptr;
    const llvm::CleanupReturnInst *Cleanup = nullptr;
    const llvm::BasicBlock *Handler = nullptr;
  };
  std::map<uint32_t, Dispatch> Dispatches;
  std::map<uint32_t, const llvm::BasicBlock *> HandlerTargets;
  using RangeKey = std::pair<uint32_t, uint32_t>;
  std::map<RangeKey, std::pair<va_t, const llvm::InvokeInst *>> Enters, Exits;
  struct BlockTarget {
    Provenance Identity;
    const llvm::CallInst *Anchor;
  };
  std::map<uint32_t, BlockTarget> BlockEntries, BlockExits;
  std::set<const llvm::InvokeInst *> Invokes;
  std::set<const llvm::CallBase *> ScopeBoundaries;
  std::set<const llvm::CallBase *> AbnormalFinallyIR;
  std::map<va_t, uint32_t> Protected;
  std::set<std::pair<va_t, int>> Chain;
  std::set<const llvm::Instruction *> ChainProtocolReads;
  size_t FSReads = 0;
  size_t ExpectedFSReads = 0;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block) {
      for (const auto &Operand : Instruction.operands())
        if (Operand->getType()->isPointerTy() &&
            Operand->getType()->getPointerAddressSpace() == 257) {
          const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
          if (!Load || Load->getPointerOperand() != Operand.get() ||
              !llvm::isa<llvm::ConstantPointerNull>(Operand) ||
              !Load->getType()->isIntegerTy(32) || Load->isAtomic() ||
              !Load->isVolatile())
            return reject("native parent has an unauthenticated FS operation");
          ++FSReads;
        }
      const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      if (!Call ||
          !Call->countOperandBundlesOfType(windows_eh_md::ProvenanceBundle))
        continue;
      auto P = coff_native_eh::parseNativeEHProvenance(*Call);
      if (!P || P->Model != Model || P->FunctionVA != Source.CodeRange.Begin)
        return reject("parent registration anchor is malformed");
      if (P->Role == Role::RegistrationChainAccess) {
        auto Found =
            llvm::find_if(States.ChainAccesses, [&](const auto &Access) {
              return Access.Address == P->SourceVA &&
                     Access.OpSeq == int(P->Clause) &&
                     Access.EndAddress == P->AuxVA &&
                     uint32_t(Access.AccessKind) == P->Flags;
            });
        if (P->Region || Call->getNumOperandBundles() != 1 ||
            Found == States.ChainAccesses.end() ||
            !Chain.emplace(P->SourceVA, P->Clause).second)
          return reject(
              "chain replacement does not match its source occurrence");
        if (Found->AccessKind ==
            RegistrationChainAccess::Kind::ReadPreviousHead) {
          const auto *Head =
              llvm::dyn_cast_or_null<llvm::LoadInst>(next(*Call));
          const auto *Pointer =
              Head ? llvm::dyn_cast_or_null<llvm::IntToPtrInst>(next(*Head))
                   : nullptr;
          const auto *Previous =
              Pointer ? llvm::dyn_cast_or_null<llvm::LoadInst>(next(*Pointer))
                      : nullptr;
          if (!Head ||
              Head->getPointerOperand()->getType()->getPointerAddressSpace() !=
                  257 ||
              !Pointer || Pointer->getOperand(0) != Head ||
              !Head->hasOneUse() || !Pointer->hasOneUse() || !Previous ||
              Previous->getPointerOperand() != Pointer ||
              !Previous->getType()->isIntegerTy(32) ||
              !Previous->isVolatile() || Previous->isAtomic() ||
              Previous->getAlign() != llvm::Align(1))
            return reject("source previous head is not loaded through the "
                          "compiler registration");
          ++ExpectedFSReads;
          ChainProtocolReads.insert(Head);
          ChainProtocolReads.insert(Previous);
        } else if (Found->AccessKind ==
                   RegistrationChainAccess::Kind::ReadInstalledHead) {
          const auto *Address = next(*Call);
          const auto *Value =
              Address
                  ? llvm::dyn_cast_or_null<llvm::PtrToIntInst>(next(*Address))
                  : nullptr;
          if (!Address || !frameAddress(Address, *Frame, Frame->EntrySP - 20) ||
              !Value || !Value->getType()->isIntegerTy(32) ||
              Value->getPointerOperand() != Address)
            return reject("source installed head no longer names its logical "
                          "registration");
        }
        continue;
      }
      if (P->Role == Role::RangeEnterTarget ||
          P->Role == Role::RangeExitTarget) {
        auto &Targets =
            P->Role == Role::RangeEnterTarget ? BlockEntries : BlockExits;
        if (P->Clause || Call->getNumOperandBundles() != 1 ||
            !Targets.emplace(P->Region, BlockTarget{*P, Call}).second)
          return reject("source registration block identity is duplicated");
        if (P->Role == Role::RangeEnterTarget) {
          const auto *End =
              llvm::dyn_cast_or_null<llvm::IntrinsicInst>(next(*Call));
          if (Call != &*Block.getFirstInsertionPt() || !End ||
              End->getIntrinsicID() != llvm::Intrinsic::seh_scope_end ||
              End->arg_size() || End->getNumOperandBundles())
            return reject(
                "inactive block does not establish its source sentinel");
          ScopeBoundaries.insert(End);
        } else if (next(*Call) != Block.getTerminator())
          return reject(
              "source block exit no longer precedes its original terminator");
        continue;
      }
      if (P->Region >= Scopes.size())
        return reject("registration anchor names an absent source scope");
      const auto &Scope = Scopes[P->Region];
      if (P->Role == Role::HandlerTarget) {
        if (Scope.IsFinally || P->SourceVA != Scope.HandlerVA || P->Clause ||
            P->AuxVA || P->Flags || Call->getNumOperandBundles() != 1 ||
            !HandlerTargets.emplace(P->Region, &Block).second)
          return reject("handler continuation does not match the source scope");
        continue;
      }
      if (P->Role == Role::ProtectedInvoke || P->Role == Role::RangeEnter ||
          P->Role == Role::RangeExit) {
        const auto *Invoke =
            llvm::dyn_cast_or_null<llvm::InvokeInst>(next(*Call));
        if (!Invoke || P->AuxVA || P->Flags ||
            Call->getNumOperandBundles() != 1 || !Invokes.insert(Invoke).second)
          return reject("protected source occurrence lost its exact invoke");
        const auto *Callee = Invoke->getCalledFunction();
        if (P->Role == Role::ProtectedInvoke) {
          if (P->Clause || (Callee && Callee->isIntrinsic()) ||
              !matchesSourceOperation(*Invoke, Source.CodeRange.Begin,
                                      P->SourceVA, 2) ||
              !Protected.emplace(P->SourceVA, P->Region).second)
            return reject("protected call has no exact source instruction");
        } else {
          const auto ID = P->Role == Role::RangeEnter
                              ? llvm::Intrinsic::seh_scope_begin
                              : llvm::Intrinsic::seh_scope_end;
          auto &Ranges = P->Role == Role::RangeEnter ? Enters : Exits;
          if (!Callee || Callee->getIntrinsicID() != ID || Invoke->arg_size() ||
              Invoke->getNumOperandBundles() ||
              !Ranges
                   .emplace(RangeKey{P->Region, P->Clause},
                            std::make_pair(P->SourceVA, Invoke))
                   .second)
            return reject("asynchronous scope boundary changed");
          ScopeBoundaries.insert(Invoke);
        }
        continue;
      }
      if (P->Role != Role::RegionDispatch || P->Clause ||
          P->SourceVA != Scope.HandlerVA ||
          P->AuxVA != (Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA) ||
          P->Flags != uint32_t(Scope.IsFinally) ||
          Call->getNumOperandBundles() != 2)
        return reject("source scope dispatch anchor changed");
      auto Bundle = Call->getOperandBundle("funclet");
      const auto *Pad =
          Bundle && Bundle->Inputs.size() == 1
              ? llvm::dyn_cast<llvm::Instruction>(Bundle->Inputs[0])
              : nullptr;
      auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
          Source, Arch::X86, P->Region);
      if (!Pad || Pad->getParent() != &Block || !Token ||
          !exactSemanticToken(*Pad, *Token))
        return reject("dispatch pad lost its source semantic token");
      const auto *Address = next(*Call);
      const auto *Sync =
          Address ? llvm::dyn_cast_or_null<llvm::StoreInst>(next(*Address))
                  : nullptr;
      if (!Address || !frameAddress(Address, *Frame, Frame->EntrySP - 8) ||
          !Sync || Sync->getPointerOperand() != Address ||
          integer(Sync->getValueOperand(), 32) !=
              uint32_t(Scope.EnclosingLevel) ||
          !Sync->isVolatile() || Sync->isAtomic() ||
          Sync->getAlign() != llvm::Align(1))
        return reject(
            "handler dispatch lost its source try-level synchronization");
      Dispatch D;
      const auto *Callback =
          CallbackMap->at({Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA,
                           Scope.IsFinally});
      if (Scope.IsFinally) {
        const auto *Cleanup = llvm::dyn_cast<llvm::CleanupPadInst>(Pad);
        D.Cleanup =
            llvm::dyn_cast<llvm::CleanupReturnInst>(Block.getTerminator());
        if (!Cleanup || !D.Cleanup || D.Cleanup->getCleanupPad() != Cleanup)
          return reject("finally dispatch lost its cleanup continuation");
        D.Unwind = &Block;
        unsigned Calls = 0;
        for (const auto &I : Block)
          if (const auto *C = llvm::dyn_cast<llvm::CallInst>(&I);
              C && C->getCalledFunction() == Callback) {
            auto F = C->getOperandBundle("funclet");
            const auto *Frame =
                C->arg_size() == 2
                    ? llvm::dyn_cast<llvm::IntrinsicInst>(C->getArgOperand(1))
                    : nullptr;
            if (C->arg_size() != 2 || integer(C->getArgOperand(0), 8) != 1 ||
                !Frame ||
                Frame->getIntrinsicID() != llvm::Intrinsic::localaddress ||
                !F || F->Inputs.size() != 1 || F->Inputs[0].get() != Pad ||
                ++Calls != 1)
              return reject("finally callback lost its abnormal parent frame");
            AbnormalFinallyIR.insert(C);
          }
        if (Calls != 1)
          return reject("finally dispatch omits its callback");
      } else {
        const auto *Catch = llvm::dyn_cast<llvm::CatchPadInst>(Pad);
        const auto *Return =
            llvm::dyn_cast<llvm::CatchReturnInst>(Block.getTerminator());
        if (!Catch || Catch->arg_size() != 1 ||
            Catch->getArgOperand(0) != Callback || !Return ||
            Return->getCatchPad() != Catch)
          return reject("filter or handler continuation changed");
        D.Switch = Catch->getCatchSwitch();
        if (D.Switch->getNumHandlers() != 1 ||
            D.Switch->getParentPad() !=
                llvm::ConstantTokenNone::get(Function.getContext()))
          return reject("scope catchswitch has extra handlers or a parent pad");
        D.Unwind = D.Switch->getParent();
        D.Handler = Return->getSuccessor();
      }
      if (!Dispatches.emplace(P->Region, D).second)
        return reject("source scope dispatch is duplicated");
    }
  if (Chain.size() != States.ChainAccesses.size() ||
      FSReads != ExpectedFSReads || Dispatches.size() != Scopes.size())
    return reject("registration chain or dispatch set is incomplete");
  std::vector<ExceptionAddressRange> ImmutableImageRanges;
  auto SourceTable = coff_loader::getX86RegistrationSEHScopeTableRange(Source);
  if (!SourceTable)
    return reject("source registration scope-table extent is invalid");
  ImmutableImageRanges.push_back(*SourceTable);
  if (Source.Personality == ExceptionPersonality::ExceptHandler4) {
    ImmutableImageRanges.push_back(
        {States.SecurityCookieVA, States.SecurityCookieVA + 4});
  }
  if (llvm::Error Error = coff_registration::validateFramePrivacy(
          Function, CallbackFunctions, Frame->Slot, ExceptionBridges,
          IncomingAccesses, SourceCallerPCWrites, ChainProtocolReads,
          States.SecurityCookieVA, ImmutableImageRanges))
    return Error;
  auto SourceSegments = validateSourceSegments(Function, ReplayMed,
                                               *CallbackMap, NativeFunctions);
  if (!SourceSegments)
    return SourceSegments.takeError();
  for (size_t I = 0; I < Scopes.size(); ++I) {
    const auto &D = Dispatches.at(I);
    const auto *Outer = Scopes[I].EnclosingLevel < 0
                            ? nullptr
                            : Dispatches.at(Scopes[I].EnclosingLevel).Unwind;
    const auto *Actual =
        D.Cleanup ? D.Cleanup->getUnwindDest() : D.Switch->getUnwindDest();
    if (Actual != Outer || (!Scopes[I].IsFinally && !HandlerTargets.count(I)))
      return reject("scope enclosing state or handler target changed");
  }
  size_t ActiveRanges = 0;
  std::map<int, const llvm::BasicBlock *> SourceEntries, SourceExits;
  std::map<va_t, uint32_t> ExpectedProtected;
  for (const auto &State : States.Blocks) {
    if (State.CallbackOnly)
      continue;
    if (State.Unknown || State.Levels.size() > 1)
      return reject("source scope is not uniquely determined at a CFG block");
    const int32_t Level = State.Levels.empty()
                              ? *Source.Registration->SeededTryLevel
                              : State.Levels.front();
    auto Target = BlockExits.find(State.BlockId);
    if (Target == BlockExits.end() ||
        Target->second.Identity.SourceVA != State.Range.End ||
        Target->second.Identity.AuxVA != State.Range.Begin ||
        Target->second.Identity.Flags != uint32_t(Level))
      return reject("source registration block has no exact exit identity");
    SourceExits.emplace(State.BlockId, Target->second.Anchor->getParent());
    const auto &Segment = SourceSegments->at(State.BlockId);
    if (Level < 0) {
      auto Entry = BlockEntries.find(State.BlockId);
      if (Entry == BlockEntries.end() ||
          Entry->second.Identity.SourceVA != State.Range.Begin ||
          Entry->second.Identity.AuxVA != State.Range.End ||
          Entry->second.Identity.Flags != uint32_t(Level) ||
          Entry->second.Anchor->getParent() !=
              Target->second.Anchor->getParent())
        return reject("inactive source block lost its exact sentinel interval");
      if (Segment.Enter->getParent() != Entry->second.Anchor->getParent() ||
          Segment.Exit->getParent() != Target->second.Anchor->getParent() ||
          !Entry->second.Anchor->comesBefore(Segment.Enter) ||
          !Segment.Exit->comesBefore(Target->second.Anchor))
        return reject(
            "inactive source execution segment lost its state interval");
      SourceEntries.emplace(State.BlockId, Entry->second.Anchor->getParent());
      continue;
    }
    ++ActiveRanges;
    const uint32_t Region = Level;
    const RangeKey Key{Region, uint32_t(State.BlockId)};
    if (!Enters.count(Key) || !Exits.count(Key) ||
        Enters.at(Key).first != State.Range.Begin ||
        Exits.at(Key).first != State.Range.End)
      return reject(
          "native protected interval disagrees with source state flow");
    const auto *Exit = Exits.at(Key).second;
    const auto *Enter = Enters.at(Key).second;
    if (Segment.Enter->getParent() != Enter->getNormalDest() ||
        Segment.Exit->getParent() != Exit->getParent() ||
        !Segment.Exit->comesBefore(Exit))
      return reject("active source execution segment lost its state interval");
    if (Exit->getNormalDest() != Target->second.Anchor->getParent() ||
        Enter->getParent()->size() != 2 ||
        !SourceEntries.emplace(State.BlockId, Enter->getParent()).second)
      return reject("active source block lost its exact boundary targets");
    if (Exit->getUnwindDest() != Dispatches.at(Region).Unwind ||
        Enter->getUnwindDest() != Dispatches.at(Region).Unwind)
      return reject("scope boundary unwinds to a different source state");
    const llvm::BasicBlock *Part = Enter->getNormalDest();
    const llvm::BasicBlock *Previous = Enter->getParent();
    std::set<const llvm::BasicBlock *> Seen;
    auto ValidateActiveCalls = [&](const llvm::BasicBlock &Part) {
      for (const auto &I : Part)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
            Call && !llvm::isa<llvm::IntrinsicInst>(Call) &&
            !llvm::isa<llvm::InvokeInst>(Call))
          return false;
      return true;
    };
    while (Part != Exit->getParent()) {
      if (!ValidateActiveCalls(*Part))
        return reject("active scope has an unanchored call that can reset its "
                      "runtime state");
      const auto *Invoke =
          llvm::dyn_cast<llvm::InvokeInst>(Part->getTerminator());
      if (Part->getSinglePredecessor() != Previous ||
          !Seen.insert(Part).second || !Invoke || !Invokes.count(Invoke) ||
          Invoke->getUnwindDest() != Dispatches.at(Region).Unwind)
        return reject("a protected interval bypasses its checked scope exit");
      Previous = Part;
      Part = Invoke->getNormalDest();
    }
    if (!ValidateActiveCalls(*Part))
      return reject("active scope has an unanchored call that can reset its "
                    "runtime state");
    if (Part->getSinglePredecessor() != Previous ||
        Exit->getNormalDest()->getSinglePredecessor() != Part)
      return reject("a protected interval bypasses its checked scope entry");
    for (const auto &B : Replay.Blocks)
      if (B.Id == State.BlockId)
        for (const auto &Op : B.Ops)
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
            if (!ExpectedProtected.emplace(Op.Addr, Region).second)
              return reject("source protected call occurrence is ambiguous");
  }
  if (ActiveRanges != Enters.size() || ActiveRanges != Exits.size() ||
      SourceEntries.size() != SourceExits.size() ||
      SourceExits.size() != BlockExits.size() ||
      SourceEntries.size() - ActiveRanges != BlockEntries.size() ||
      ExpectedProtected != Protected)
    return reject("native scope ranges or protected call set is incomplete");
  for (size_t I = 0; I < Scopes.size(); ++I) {
    if (Scopes[I].IsFinally)
      continue;
    const RegistrationBlockState *HandlerState = nullptr;
    for (const auto &State : States.Blocks)
      if (State.Range.Begin == Scopes[I].HandlerVA && !State.CallbackOnly)
        HandlerState = &State;
    if (!HandlerState || !SourceEntries.count(HandlerState->BlockId))
      return reject("handler target has no checked source block");
    const auto *Entry = SourceEntries.at(HandlerState->BlockId);
    const llvm::BasicBlock *Body = Entry;
    if (!HandlerState->Levels.empty() && HandlerState->Levels.front() >= 0)
      Body = Enters
                 .at({uint32_t(HandlerState->Levels.front()),
                      uint32_t(HandlerState->BlockId)})
                 .second->getNormalDest();
    // A nested except body remains inside its outer scope. Its catchret must
    // enter the authenticated scope-begin wrapper before reaching that body.
    if (Dispatches.at(I).Handler != Entry || HandlerTargets.at(I) != Body)
      return reject("scope handler target bypasses its checked source entry");
  }
  for (const auto &Block : Replay.Blocks) {
    auto Entry = SourceEntries.find(Block.Id);
    if (Entry == SourceEntries.end())
      continue;
    if (Block.StartAddr == Source.CodeRange.Begin &&
        Entry->second != &Function.getEntryBlock()) {
      const auto &Setup = Function.getEntryBlock();
      const auto *Branch =
          llvm::dyn_cast<llvm::UncondBrInst>(Setup.getTerminator());
      if (!Branch || Branch->getSuccessor(0) != Entry->second)
        return reject("machine entry bypasses its checked registration block");
      for (const auto &I : Setup) {
        if (IncomingSetup.count(&I))
          continue;
        if (I.isTerminator() || llvm::isa<llvm::AllocaInst>(I) ||
            llvm::isa<llvm::GetElementPtrInst>(I) ||
            llvm::isa<llvm::CastInst>(I))
          continue;
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
            Store && !Store->isAtomic() &&
            llvm::isa<llvm::AllocaInst>(Store->getPointerOperand()))
          continue;
        if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
            Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape)
          continue;
        return reject("frame setup contains an unowned source operation");
      }
    }
    std::set<const llvm::BasicBlock *> Expected;
    for (int Successor : Block.Succs) {
      auto Destination = SourceEntries.find(Successor);
      if (Destination == SourceEntries.end())
        return reject(
            "source normal edge enters an unowned registration block");
      Expected.insert(Destination->second);
    }
    const auto *Terminator = SourceExits.at(Block.Id)->getTerminator();
    std::set<const llvm::BasicBlock *> Actual;
    for (unsigned I = 0; I < Terminator->getNumSuccessors(); ++I)
      Actual.insert(Terminator->getSuccessor(I));
    if (Actual != Expected)
      return reject(
          "native normal control flow bypasses a source registration boundary");
  }
  for (const auto *NativeFunction : NativeFunctions)
    for (const auto &Block : *NativeFunction)
      for (const auto &I : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
            Call && Call->getCalledFunction() &&
            (Call->getCalledFunction()->getIntrinsicID() ==
                 llvm::Intrinsic::seh_scope_begin ||
             Call->getCalledFunction()->getIntrinsicID() ==
                 llvm::Intrinsic::seh_scope_end) &&
            !ScopeBoundaries.count(Call))
          return reject(
              "native parent contains an unanchored SEH scope state change");
  for (const auto &Block : Function)
    if (const auto *Invoke =
            llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
        Invoke && !Invokes.count(Invoke))
      return reject("native parent contains an unanchored exceptional edge");
  for (const auto *NativeFunction : NativeFunctions)
    for (const auto &Block : *NativeFunction)
      for (const auto &I : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
            Call &&
            llvm::is_contained(CallbackFunctions, Call->getCalledFunction()) &&
            !NormalFinallyIR.count(Call) && !AbnormalFinallyIR.count(Call))
          return reject(
              "generated callback has no checked runtime entry protocol");
  LowFunc GeneratedCalleeABI;
  GeneratedCalleeABI.Entry = Source.CodeRange.Begin;
  GeneratedCalleeABI.ExceptionMetadata = Source;
  GeneratedCalleeABI.Blocks.emplace_back();
  for (const auto *NativeFunction : NativeFunctions)
    for (const auto &Block : *NativeFunction)
      for (const auto &I : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!Call)
          continue;
        const auto *Callee = Call->getCalledFunction();
        if (Callee && Callee->isIntrinsic())
          continue;
        if (!Callee || Call->getCallingConv() != llvm::CallingConv::C ||
            Callee->getCallingConv() != llvm::CallingConv::C ||
            Callee->hasFnAttribute(llvm::Attribute::Naked))
          return reject(
              "generated callee has an unproved x86 stack cleanup ABI");
        if (!Callee->isDeclaration())
          continue;
        auto Address = rewrite_source::getOriginalVA(*Callee);
        if (!Address)
          return Address.takeError();
        if (!*Address)
          return reject(
              "generated external call has no checked source stack ABI");
        for (const auto &Scope : Source.Registration->Scopes)
          if (**Address == Scope.FilterVA || **Address == Scope.HandlerVA)
            return reject("original callback code has no recovered runtime "
                          "entry protocol");
        LowOp Op;
        Op.Opcode = NdOp::CALL;
        Op.NumInputs = 1;
        Op.Inputs[0] = NdVar::cst(**Address, 4);
        GeneratedCalleeABI.Blocks.front().Ops.push_back(Op);
      }
  std::vector<ExceptionAddressRange> GeneratedCallerPCWrites;
  if (!hasCallerCleanupRegistrationABI(GeneratedCalleeABI, Image,
                                       &GeneratedCallerPCWrites))
    return reject(
        "generated preserved callee has an unproved x86 stack cleanup ABI");
  if (GeneratedCallerPCWrites.size() != SourceCallerPCWrites.size() ||
      !std::equal(GeneratedCallerPCWrites.begin(),
                  GeneratedCallerPCWrites.end(), SourceCallerPCWrites.begin(),
                  [](const auto &A, const auto &B) {
                    return A.Begin == B.Begin && A.End == B.End;
                  }))
    if (auto Error = coff_registration::validateFramePrivacy(
            Function, CallbackFunctions, Frame->Slot, ExceptionBridges,
            IncomingAccesses, GeneratedCallerPCWrites, ChainProtocolReads,
            States.SecurityCookieVA, ImmutableImageRanges))
      return Error;
  return llvm::Error::success();
#endif
}

llvm::Error
validateCOFFRegistrationSemanticRows(const llvm::Function &Function,
                                     const ExceptionFunction &Source,
                                     const CompiledImage &Compiled) {
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return reject("LLVM does not provide indexed x86 SEH output receipts");
#else
  if (!Source.Registration || !Function.hasPersonalityFn() ||
      !llvm::isa<llvm::GlobalValue>(Function.getPersonalityFn()) ||
      !classifyWindowsEHNativeSource(Source, Arch::X86, BinaryFormat::COFF,
                                     WindowsEHNativeCapability::IRLowering)
           .canLowerNativeIR() ||
      !Compiled.Success || !Compiled.FunctionRangesValid ||
      !Compiled.WinEHSemanticsValid || Compiled.TargetArch != Arch::X86 ||
      Compiled.Format != BinaryFormat::COFF || Compiled.PointerWidth != 4 ||
      Compiled.ByteOrder != llvm::endianness::little ||
      llvm::Triple(Compiled.TargetTriple).getArch() != llvm::Triple::x86 ||
      !llvm::Triple(Compiled.TargetTriple).isWindowsMSVCEnvironment() ||
      !llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
          Compiled.WinEHSemanticRecords, Compiled.SourceFunctionOwners,
          Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs))
    return reject("compiler x86 SEH receipts are incomplete or inconsistent");
  auto Root = ownerVA(Function, Compiled);
  if (!Root)
    return Root.takeError();
  auto CallbackMap = callbacks(Function, Source);
  if (!CallbackMap)
    return CallbackMap.takeError();
  const auto &Chain = *Source.Registration;
  const bool EH4 = Source.Personality == ExceptionPersonality::ExceptHandler4;
  const int32_t Sentinel = EH4 ? -2 : -1;
  std::map<uint32_t, const CompiledWinEHSemanticRecord *> ByState, ByRegion;
  uint64_t ContainerVA = 0;
  uint64_t ContainerEndVA = 0;
  std::string ContainerSymbol;
  for (const auto &Row : Compiled.WinEHSemanticRecords) {
    if (Row.SourceFunction != Function.getName())
      continue;
    auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
        Source, Arch::X86, Row.Token.Region);
    if (!Token || Row.Token != *Token || Row.OwnerVA != *Root ||
        Row.Token.Clause || Row.Token.Region >= Chain.Scopes.size() ||
        Row.Encoding !=
            (EH4 ? llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86SEH4
                 : llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86SEH3) ||
        Row.RecordSize != 12 ||
        !ByState.emplace(Row.GeneratedState, &Row).second ||
        !ByRegion.emplace(Row.Token.Region, &Row).second)
      return reject(
          "generated scope row has no exact source semantic identity");
    if (!ContainerVA) {
      ContainerVA = Row.ContainerVA;
      ContainerEndVA = Row.ContainerEndVA;
      ContainerSymbol = Row.ContainerSymbol;
    }
    if (ContainerVA != Row.ContainerVA ||
        ContainerEndVA != Row.ContainerEndVA ||
        ContainerSymbol != Row.ContainerSymbol)
      return reject(
          "one source registration has multiple generated scope tables");
    const auto *Section =
        sectionAt(Compiled, Row.RecordVA, Row.RecordSize,
                  llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData);
    if (!Section || !sectionAt(Compiled, Row.HandlerVA, 1,
                               llvm::mc_rewrite::RewriteSectionKind::Code))
      return reject("scope row or handler has no exact generated section");
    const auto *Bytes = Compiled.Bytes.data() + Row.RecordVA - Compiled.BaseVA;
    if (readLE<int32_t>(Bytes) != Row.EnclosingState ||
        readLE<uint32_t>(Bytes + 4) != Row.FilterVA ||
        readLE<uint32_t>(Bytes + 8) != Row.HandlerVA ||
        !exactPointerFixup(*Section, Row.RecordVA + 8, Row.HandlerSymbol,
                           Row.HandlerVA))
      return reject(
          "scope row bytes disagree with compiler pointer provenance");
    const auto &Scope = Chain.Scopes[Row.Token.Region];
    if (Scope.IsFinally) {
      if (Row.FilterVA || !Row.FilterSymbol.empty())
        return reject("finally row acquired a searching filter");
    } else {
      auto Filter =
          ownerVA(*CallbackMap->at({Scope.FilterVA, false}), Compiled);
      if (!Filter)
        return Filter.takeError();
      if (Row.FilterVA != *Filter ||
          !exactPointerFixup(*Section, Row.RecordVA + 4, Row.FilterSymbol,
                             *Filter))
        return reject("scope filter does not name its recovered callback");
    }
  }
  if (ByState.size() != Chain.Scopes.size() ||
      ByRegion.size() != Chain.Scopes.size() || ContainerEndVA <= ContainerVA ||
      ContainerEndVA - ContainerVA != (EH4 ? 16u : 0u) + 12 * ByState.size() ||
      !sectionAt(Compiled, ContainerVA, ContainerEndVA - ContainerVA,
                 llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData))
    return reject("generated indexed scope table is not physically closed");
  for (uint32_t I = 0; I < ByState.size(); ++I) {
    if (!ByState.count(I))
      return reject("generated scope state numbering has a hole");
    const auto &Row = *ByState.at(I);
    const int32_t SourceOuter = Chain.Scopes[Row.Token.Region].EnclosingLevel;
    if (Row.RecordVA != ContainerVA + (EH4 ? 16 : 0) + 12 * I ||
        (SourceOuter < 0
             ? Row.EnclosingState != Sentinel
             : (Row.EnclosingState < 0 || !ByState.count(Row.EnclosingState) ||
                ByState.at(Row.EnclosingState)->Token.Region !=
                    uint32_t(SourceOuter))))
      return reject("generated enclosing state changed the source scope graph");
  }
  if (EH4) {
    const auto *Header = Compiled.Bytes.data() + ContainerVA - Compiled.BaseVA;
    const auto &Cookies = ByState.begin()->second->RegistrationCookieOffsets;
    for (unsigned I = 0; I != Cookies.size(); ++I)
      if (readLE<int32_t>(Header + I * 4) != Cookies[I])
        return reject(
            "generated EH4 cookie header changed its machine frame offsets");
    const int32_t GS = readLE<int32_t>(Header);
    const int32_t EH = readLE<int32_t>(Header + 8);
    if ((Chain.GSCookieOffset == -2
             ? GS != -2 || readLE<uint32_t>(Header + 4)
             : GS == -2 || GS % 4 || readLE<int32_t>(Header + 4) % 4) ||
        EH % 4 || readLE<uint32_t>(Header + 12))
      return reject(
          "generated EH4 cookie header is not compiler frame derived");
  }
  bool TableReferenced = false;
  bool PersonalityReferenced = false;
  llvm::SmallString<64> PersonalitySymbol;
  llvm::Mangler Mangler;
  Mangler.getNameWithPrefix(
      PersonalitySymbol,
      llvm::cast<llvm::GlobalValue>(Function.getPersonalityFn()), false);
  for (const auto &Section : Compiled.Sections) {
    if (Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code)
      continue;
    for (const auto &Fixup : Section.FixupReferences) {
      if (!absolutePointer(Fixup) || Fixup.Addend)
        continue;
      bool InRoot = false;
      for (const auto &Range : Compiled.FunctionRanges)
        if (Range.OwnerVA == *Root &&
            Section.VA + Fixup.Offset >= Range.BeginVA &&
            Section.VA + Fixup.Offset + 4 <= Range.EndVA)
          InRoot = true;
      if (!InRoot)
        continue;
      TableReferenced |=
          Fixup.Symbol == ContainerSymbol && Fixup.ResolvedValue == ContainerVA;
      PersonalityReferenced |= Fixup.Symbol == PersonalitySymbol &&
                               Fixup.ResolvedValue == Source.PersonalityVA;
    }
  }
  if (!TableReferenced || !PersonalityReferenced)
    return reject(
        "live registration node does not reference its exact native contract");
  return llvm::Error::success();
#endif
}

llvm::Expected<COFFRegistrationPatchUpdate> prepareCOFFRegistrationPatch(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, const llvm::Module &RewriteModule,
    const COFFGuardTableUpdate *GuardUpdate) {
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return reject("LLVM does not provide indexed x86 SEH output receipts");
#else
  COFFRegistrationPatchUpdate Update;
  std::map<va_t, va_t> Mappings;
  std::map<va_t, const ExceptionFunction *> Sources;
  for (const auto &[Original, Generated] : PatchedEntryMappings) {
    if (Original < Image.Base || Original > uint64_t(UINT32_MAX) - 4 ||
        !Image.isCodeAddress(Original) || Generated < NewSectionVA ||
        Generated > UINT32_MAX)
      return reject("patched entry mapping lies outside its executable image");
    if (!Mappings.emplace(Original, Generated).second)
      return reject("patched entry mapping is duplicated");
    for (const auto &EH : Image.ExceptionMetadata.Functions)
      if (EH.CodeRange.Begin == Original) {
        if (!EH.Registration || !Sources.emplace(Original, &EH).second)
          return reject(
              "patched x86 exception function has no unique registration");
      }
  }
  if (Sources.empty())
    return Update;
  auto PE = locatePEHeaders(const_cast<uint8_t *>(OriginalBinary.data()),
                            OriginalBinary.size());
  if (!PE.valid() || PE.Is64 ||
      PE.FileHeader->Machine != llvm::COFF::IMAGE_FILE_MACHINE_I386 ||
      getPEImageBase(PE) != Image.Base || Image.Arch != Arch::X86 ||
      Image.Format != BinaryFormat::COFF || Compiled.BaseVA != NewSectionVA ||
      NewSectionVA < Image.Base || NewSectionVA > UINT32_MAX ||
      Compiled.Bytes.size() > UINT32_MAX ||
      Compiled.Bytes.size() > uint64_t(UINT32_MAX) + 1 - NewSectionVA ||
      !Compiled.Success || !Compiled.Unresolved.empty())
    return reject("generated x86 image does not match its PE32 placement");
  const auto *Optional = getPE32OptionalHeader(PE);
  Update.ImageBase = Image.Base;
  Update.DllCharacteristics = Optional->DLLCharacteristics;
  Update.FileCharacteristics = PE.FileHeader->Characteristics;
  if (Update.DllCharacteristics & llvm::COFF::IMAGE_DLL_CHARACTERISTICS_NO_SEH)
    return reject(
        "input forbids SEH while containing a live registration contract");
  const bool Fixed =
      Update.FileCharacteristics & llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED;
  if (Fixed && (Update.DllCharacteristics &
                llvm::COFF::IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE))
    return reject("input combines stripped relocations with dynamic base");
  if (!Fixed)
    if (llvm::Error Error = validateSymbolicImagePointers(
            RewriteModule, Image.Base, getPESizeOfImage(PE)))
      return std::move(Error);
  std::set<uint32_t> RequiredHandlers;
  std::set<uint32_t> OriginalHandlers;
  std::set<uint32_t> GeneratedHandlers;
  std::set<va_t> RequiredAbsoluteFields;
  std::set<std::string> SourceNames;
  for (const auto &[Original, EH] : Sources) {
    const llvm::Function *Function = nullptr;
    for (const auto &[Name, VA] : Compiled.SourceFunctionOriginalVAs)
      if (VA == Original) {
        if (Function || !RewriteModule.getFunction(Name))
          return reject("source entry has no unique generated IR definition");
        Function = RewriteModule.getFunction(Name);
      }
    if (!Function || Function->isDeclaration())
      return reject("patched registration source has no generated definition");
    auto Owner = ownerVA(*Function, Compiled);
    if (!Owner)
      return Owner.takeError();
    if (*Owner != Mappings.at(Original))
      return reject(
          "entry trampoline names a different compiler function owner");
    if (EH->Cxx) {
      if (llvm::Error Error =
              validateCOFFRegistrationCxxIR(*Function, *EH, Image))
        return std::move(Error);
      auto Proof =
          coff_registration::getCheckedCxxControlIRProof(*Function, *EH, Image);
      if (!Proof)
        return Proof.takeError();
      size_t CalleeWork = 0;
      for (const auto &[Call, Checked] : Proof->Calls) {
        if (Checked.Contract.CodeRanges.empty())
          return reject("preserved C++ callee has no complete code extent");
        for (const auto &[Entry, Generated] : Mappings)
          for (const auto &Range : Checked.Contract.CodeRanges) {
            if (++CalleeWork > limits::kMaxRegistrationEHStateWork)
              return reject("C++ preserved code proof exceeds its work budget");
            if (!Range.isValid() || Range.End > uint64_t(UINT32_MAX) + 1 ||
                ExceptionAddressRange{Entry, Entry + 5}.overlaps(Range))
              return reject("entry trampoline overwrites preserved C++ callee "
                            "code without a native replacement contract");
          }
      }
      auto Runtime = coff_loader::getCheckedX86CxxPersonalityABI(Image, *EH);
      if (!Runtime)
        return reject("preserved C++ CRT dispatch has no original ABI");
      for (const auto &[Entry, Generated] : Mappings)
        for (const auto &Range : Runtime->CodeRanges)
          if (ExceptionAddressRange{Entry, Entry + 5}.overlaps(Range))
            return reject("entry trampoline overwrites preserved C++ CRT "
                          "dispatch code");
      auto Receipt = getCheckedCOFFRegistrationCxxHandlerReceipt(
          *Function, *EH, Image, Compiled);
      if (!Receipt)
        return Receipt.takeError();
      Update.GeneratedCxxGraphs.push_back(Receipt->Tables.GeneratedCxxGraph);
      if (Receipt->CodeRange.Begin < NewSectionVA ||
          Receipt->CodeRange.Begin < Image.Base ||
          Receipt->CodeRange.Begin > UINT32_MAX)
        return reject(
            "generated C++ registration handler is outside its image");
      const uint32_t RVA = Receipt->CodeRange.Begin - Image.Base;
      GeneratedHandlers.insert(RVA);
      RequiredHandlers.insert(RVA);
      RequiredAbsoluteFields.insert(Receipt->AbsolutePointerFields.begin(),
                                    Receipt->AbsolutePointerFields.end());
    } else {
      if (llvm::Error Error = validateCOFFRegistrationIR(*Function, *EH, Image))
        return std::move(Error);
      if (llvm::Error Error =
              validateCOFFRegistrationSemanticRows(*Function, *EH, Compiled))
        return std::move(Error);
      RequiredHandlers.insert(uint32_t(EH->PersonalityVA - Image.Base));
    }
    SourceNames.insert(Function->getName().str());
    if (EH->PersonalityVA < Image.Base || EH->PersonalityVA > UINT32_MAX ||
        !Image.isCodeAddress(EH->PersonalityVA) ||
        !Image.readVA(EH->PersonalityVA, 1))
      return reject(
          "source registration personality is not executable image code");
    OriginalHandlers.insert(uint32_t(EH->PersonalityVA - Image.Base));
    if (EH->Personality == ExceptionPersonality::ExceptHandler4) {
      const uint64_t Cookie = Image.Base + Image.DynInfo.SecurityCookieRVA;
      if (!Image.DynInfo.SecurityCookieRVA || Cookie > UINT32_MAX ||
          !Image.readVA(Cookie, 4))
        return reject(
            "EH4 source has no authenticated security cookie storage");
      bool Referenced = false;
      for (const auto &Section : Compiled.Sections)
        for (const auto &Fixup : Section.FixupReferences)
          Referenced |= absolutePointer(Fixup) && Fixup.ResolvedValue == Cookie;
      if (!Referenced)
        return reject("generated EH4 frame omits the input security cookie");
    }
  }
  for (const auto &Row : Compiled.WinEHSemanticRecords)
    if (!SourceNames.count(Row.SourceFunction))
      return reject(
          "generated scope row belongs to an uninstalled source owner");

  auto Prepared = Compiled.Bytes;
  auto Append = [&](llvm::ArrayRef<uint8_t> Bytes) -> llvm::Expected<uint32_t> {
    const uint64_t Aligned = llvm::alignTo(Prepared.size(), uint64_t(4));
    if (Aligned > UINT32_MAX || Bytes.size() > UINT32_MAX - Aligned ||
        Aligned + Bytes.size() > uint64_t(UINT32_MAX) + 1 - NewSectionVA)
      return reject("registration metadata exceeds the PE32 address domain");
    Prepared.resize(Aligned, 0);
    const uint32_t RVA = uint32_t(NewSectionVA - Image.Base + Aligned);
    Prepared.insert(Prepared.end(), Bytes.begin(), Bytes.end());
    return RVA;
  };
  std::set<uint32_t> Relocations;
  const auto *RelocDirectory =
      getPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE);
  const bool HasRelocations = RelocDirectory && RelocDirectory->Size &&
                              RelocDirectory->RelativeVirtualAddress;
  if (!RelocDirectory ||
      bool(RelocDirectory->Size) !=
          bool(RelocDirectory->RelativeVirtualAddress) ||
      (Fixed && HasRelocations) || (!Fixed && !HasRelocations))
    return reject("input relocation contract is missing or contradictory");
  if (HasRelocations) {
    auto Offset =
        rawOffset(PE, OriginalBinary, RelocDirectory->RelativeVirtualAddress,
                  RelocDirectory->Size);
    if (!Offset)
      return Offset.takeError();
    const auto Bytes = OriginalBinary.slice(*Offset, RelocDirectory->Size);
    size_t Cursor = 0;
    size_t Count = 0;
    while (Cursor < Bytes.size()) {
      if (!rangeInBounds(Cursor, 8, Bytes.size()))
        return reject("input relocation block header is truncated");
      const uint32_t Page = readLE<uint32_t>(Bytes.data() + Cursor);
      const uint32_t Size = readLE<uint32_t>(Bytes.data() + Cursor + 4);
      if ((Page & 0xfff) || Size < 8 || (Size & 3) ||
          !rangeInBounds(Cursor, Size, Bytes.size()))
        return reject("input relocation block has an invalid extent");
      for (size_t I = 8; I < Size; I += 2) {
        if (++Count > limits::kMaxRegistrationEHStateWork)
          return reject("input relocation directory exceeds the work budget");
        const uint16_t Entry = readLE<uint16_t>(Bytes.data() + Cursor + I);
        const unsigned Type = Entry >> 12;
        if (!Type)
          continue;
        if (Type != llvm::COFF::IMAGE_REL_BASED_HIGHLOW ||
            uint64_t(Page) + (Entry & 0xfff) > UINT32_MAX)
          return reject("input has an unsupported PE32 relocation kind");
        const uint32_t RVA = Page + (Entry & 0xfff);
        auto Field = rawOffset(PE, OriginalBinary, RVA, 4);
        if (!Field)
          return Field.takeError();
        bool Removed = false;
        for (const auto &[Original, Generated] : Mappings) {
          const uint64_t Begin = Original - Image.Base;
          if (uint64_t(RVA) < Begin + 5 && Begin < uint64_t(RVA) + 4) {
            if (RVA < Begin || uint64_t(RVA) + 4 > Begin + 5)
              return reject(
                  "entry trampoline partially overwrites a relocation field");
            Removed = true;
          }
        }
        if (!Removed && !Relocations.insert(RVA).second)
          return reject("input relocation directory contains duplicate fields");
      }
      Cursor += Size;
    }
  }
  std::map<va_t, const llvm::Function *> EntryOwners;
  if (Compiled.SourceFunctionOriginalVAs.size() >
      limits::kMaxRegistrationEHStateWork)
    return reject("patched entry owner set exceeds its work budget");
  for (const auto &[Name, Original] : Compiled.SourceFunctionOriginalVAs) {
    if (!Mappings.count(Original))
      continue;
    const auto *Function = RewriteModule.getFunction(Name);
    if (!Function || Function->isDeclaration() ||
        !EntryOwners.emplace(Original, Function).second)
      return reject("patched entry has no unique compiled source definition");
  }
  for (const auto &[Original, Generated] : Mappings) {
    const auto Owner = EntryOwners.find(Original);
    if (Owner == EntryOwners.end())
      return reject("patched entry has no exact compiler source owner");
    auto Address = ownerVA(*Owner->second, Compiled);
    if (!Address)
      return Address.takeError();
    if (*Address != Generated)
      return reject("patched entry target differs from its compiler owner");
    auto Offset = rawOffset(PE, OriginalBinary, Original - Image.Base, 5);
    if (!Offset)
      return Offset.takeError();
    Update.PatchedEntryRVAs.emplace_back(Original - Image.Base,
                                         Generated - Image.Base);
    const auto Source = Sources.find(Original);
    Update.EntryEncodings.push_back(
        Source == Sources.end()
            ? std::nullopt
            : std::optional<ExceptionEncoding>(Source->second->Encoding));
  }
  std::set<va_t> AbsoluteFields;
  for (const auto &Section : Compiled.Sections) {
    if (Section.Name == ".sxdata") {
      if (Section.IsAllocated || Section.IsInImage || Section.Size % 4 ||
          Section.ExternalBytes.size() != Section.Size ||
          Section.SymbolIndexReferences.size() != Section.Size / 4)
        return reject("SafeSEH linker metadata is not physically closed");
      std::set<uint64_t> Offsets;
      for (const auto &Reference : Section.SymbolIndexReferences) {
        if (Reference.Offset % 4 ||
            !rangeInBounds(Reference.Offset, 4, Section.Size) ||
            !Offsets.insert(Reference.Offset).second ||
            Reference.Symbol.empty() || Reference.TargetVA < Image.Base ||
            Reference.TargetVA > UINT32_MAX ||
            !RequiredHandlers.count(uint32_t(Reference.TargetVA - Image.Base)))
          return reject(
              "SafeSEH linker row names an unproven registration handler");
      }
      continue;
    }
    if (!Section.Size) {
      if (!Section.FixupReferences.empty() ||
          !Section.SymbolIndexReferences.empty())
        return reject("empty generated section carries address fixups");
      continue;
    }
    if (!Section.IsAllocated)
      continue;
    if (!Section.IsInImage || Section.VA < NewSectionVA ||
        Section.Offset != Section.VA - NewSectionVA ||
        !rangeInBounds(Section.Offset, Section.Size, Compiled.Bytes.size()) ||
        (Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code &&
         Section.Kind != llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData))
      return reject("generated registration section has incompatible placement "
                    "or permissions");
    for (const auto &Fixup : Section.FixupReferences) {
      if (Fixup.IsPCRel || !Fixup.SubtractSymbol.empty())
        continue;
      if (!absolutePointer(Fixup) ||
          !rangeInBounds(Fixup.Offset, 4, Section.Size) ||
          Fixup.ResolvedValue > UINT32_MAX ||
          readLE<uint32_t>(Compiled.Bytes.data() + Section.Offset +
                           Fixup.Offset) != Fixup.ResolvedValue)
        return reject(
            "generated absolute address has no PE32 HIGHLOW contract");
      const va_t Field = Section.VA + Fixup.Offset;
      auto Next = AbsoluteFields.lower_bound(Field);
      if ((Next != AbsoluteFields.end() && *Next < Field + 4) ||
          (Next != AbsoluteFields.begin() && *std::prev(Next) + 4 > Field))
        return reject("generated absolute fixup fields overlap");
      AbsoluteFields.insert(Next, Field);
      if (!Fixed)
        if (!Relocations
                 .insert(uint32_t(Section.VA + Fixup.Offset - Image.Base))
                 .second)
          return reject("generated absolute fixup fields overlap");
    }
  }
  for (va_t Field : RequiredAbsoluteFields)
    if (!AbsoluteFields.count(Field))
      return reject("C++ dispatch pointer has no complete HIGHLOW closure");

  const auto *LoadConfig =
      getPEDataDirectory(PE, llvm::COFF::LOAD_CONFIG_TABLE);
  if (LoadConfig &&
      (bool(LoadConfig->Size) != bool(LoadConfig->RelativeVirtualAddress)))
    return reject("input load configuration has a contradictory extent");
  if (LoadConfig && LoadConfig->Size) {
    Update.LoadConfigRVA = LoadConfig->RelativeVirtualAddress;
    Update.LoadConfigSize = LoadConfig->Size;
    auto ConfigOffset =
        rawOffset(PE, OriginalBinary, LoadConfig->RelativeVirtualAddress,
                  LoadConfig->Size);
    if (!ConfigOffset)
      return ConfigOffset.takeError();
    if (LoadConfig->Size < 4 ||
        readLE<uint32_t>(OriginalBinary.data() + *ConfigOffset) < 4)
      return reject("input load configuration is truncated");
    Update.LoadConfigDeclaredSize =
        readLE<uint32_t>(OriginalBinary.data() + *ConfigOffset);
    // MSVC keeps the directory's compatibility size at 64 while the structure
    // declares its actual append-only ABI. The loader already owns that rule;
    // require its complete extent and a unique raw-backed section here.
    if (Image.DynInfo.LoadConfigRVA != Update.LoadConfigRVA ||
        Image.DynInfo.LoadConfigSize != Update.LoadConfigDeclaredSize)
      return reject("input load configuration differs from its loaded extent");
    ConfigOffset = rawOffset(PE, OriginalBinary, Update.LoadConfigRVA,
                             Update.LoadConfigDeclaredSize);
    if (!ConfigOffset)
      return ConfigOffset.takeError();
    const auto Config =
        OriginalBinary.slice(*ConfigOffset, Update.LoadConfigDeclaredSize);
    using LC = llvm::object::coff_load_configuration32;
    if (GuardUpdate) {
      const std::tuple<bool, uint64_t, size_t> GuardFields[] = {
          {GuardUpdate->ApplyCF, GuardUpdate->CFFunctionCount,
           offsetof(LC, GuardCFFunctionTable)},
          {GuardUpdate->ApplyEHCont, GuardUpdate->EHContinuationCount,
           offsetof(LC, GuardEHContinuationTable)}};
      for (const auto &[Apply, Count, Offset] : GuardFields) {
        if (!Apply)
          continue;
        if (Offset + 4 > Update.LoadConfigDeclaredSize ||
            uint64_t(Update.LoadConfigRVA) + Offset > UINT32_MAX)
          return reject(
              "guard pointer has no complete PE32 load configuration field");
        if (!Fixed) {
          const uint32_t Field = Update.LoadConfigRVA + Offset;
          if (Count)
            Relocations.insert(Field);
          else
            Relocations.erase(Field);
        }
      }
    }
    constexpr size_t TableOffset = offsetof(LC, SEHandlerTable);
    constexpr size_t CountOffset = offsetof(LC, SEHandlerCount);
    if (readLE<uint32_t>(Config.data()) >= CountOffset + 4) {
      const uint32_t Table = readLE<uint32_t>(Config.data() + TableOffset);
      const uint32_t Count = readLE<uint32_t>(Config.data() + CountOffset);
      if (bool(Table) != bool(Count) ||
          Count > limits::kMaxRegistrationEHStateWork)
        return reject("input SafeSEH table has a contradictory extent");
      Update.LoadConfigBytes.assign(Config.begin() + TableOffset,
                                    Config.begin() + CountOffset + 4);
      if (Count) {
        if (Table < Image.Base)
          return reject("input SafeSEH table is outside the image");
        auto Offset =
            rawOffset(PE, OriginalBinary, Table - Image.Base, Count * 4);
        if (!Offset)
          return Offset.takeError();
        for (uint32_t I = 0; I < Count; ++I) {
          const uint32_t RVA =
              readLE<uint32_t>(OriginalBinary.data() + *Offset + I * 4);
          if ((I && RVA <= Update.SafeSEHHandlers.back()) ||
              !Image.isCodeAddress(Image.Base + RVA) ||
              !Image.readVA(Image.Base + RVA, 1))
            return reject(
                "input SafeSEH handlers are not ordered executable RVAs");
          Update.SafeSEHHandlers.push_back(RVA);
        }
        for (uint32_t Handler : OriginalHandlers)
          if (!llvm::is_contained(Update.SafeSEHHandlers, Handler))
            return reject(
                "registered source personality is absent from SafeSEH");
        for (uint32_t Handler : GeneratedHandlers)
          Update.SafeSEHHandlers.push_back(Handler);
        llvm::sort(Update.SafeSEHHandlers);
        Update.SafeSEHHandlers.erase(std::unique(Update.SafeSEHHandlers.begin(),
                                                 Update.SafeSEHHandlers.end()),
                                     Update.SafeSEHHandlers.end());
        if (Update.SafeSEHHandlers.size() > limits::kMaxRegistrationEHStateWork)
          return reject("merged SafeSEH handlers exceed the work budget");
        std::vector<uint8_t> TableBytes(Update.SafeSEHHandlers.size() * 4);
        for (size_t I = 0; I < Update.SafeSEHHandlers.size(); ++I)
          writeLE<uint32_t>(TableBytes.data() + I * 4,
                            Update.SafeSEHHandlers[I]);
        auto RVA = Append(TableBytes);
        if (!RVA)
          return RVA.takeError();
        writeLE<uint32_t>(Update.LoadConfigBytes.data(), Image.Base + *RVA);
        writeLE<uint32_t>(Update.LoadConfigBytes.data() + 4,
                          Update.SafeSEHHandlers.size());
        if (!Fixed)
          Relocations.insert(Update.LoadConfigRVA + TableOffset);
      }
    }
  }
  if (!Fixed) {
    std::vector<uint8_t> RelocBytes;
    auto It = Relocations.begin();
    while (It != Relocations.end()) {
      const uint32_t Page = *It & ~uint32_t(0xfff);
      const size_t Begin = RelocBytes.size();
      RelocBytes.resize(Begin + 8);
      writeLE<uint32_t>(RelocBytes.data() + Begin, Page);
      while (It != Relocations.end() && (*It & ~uint32_t(0xfff)) == Page) {
        const size_t Offset = RelocBytes.size();
        RelocBytes.resize(Offset + 2);
        writeLE<uint16_t>(RelocBytes.data() + Offset,
                          uint16_t(llvm::COFF::IMAGE_REL_BASED_HIGHLOW << 12) |
                              uint16_t(*It & 0xfff));
        ++It;
      }
      RelocBytes.resize(llvm::alignTo(RelocBytes.size(), size_t(4)), 0);
      writeLE<uint32_t>(RelocBytes.data() + Begin + 4,
                        RelocBytes.size() - Begin);
    }
    auto RVA = Append(RelocBytes);
    if (!RVA)
      return RVA.takeError();
    Update.RelocationRVA = *RVA;
    Update.RelocationSize = RelocBytes.size();
  }
  Update.Apply = true;
  Update.SectionRVA = NewSectionVA - Image.Base;
  Update.SectionSize = Prepared.size();
  Update.SectionSHA256 = llvm::SHA256::hash(Prepared);
  Compiled.Bytes.swap(Prepared);
  return Update;
#endif
}

llvm::Error
applyCOFFRegistrationPatch(std::vector<uint8_t> &Binary,
                           const COFFRegistrationPatchUpdate &Update) {
  if (!Update.Apply)
    return llvm::Error::success();
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return reject("LLVM does not provide indexed x86 SEH output receipts");
#else
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  if (!PE.valid() || PE.Is64 ||
      !getPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE))
    return reject(
        "final PE32 cannot install the registration relocation contract");
  if (!Update.LoadConfigBytes.empty()) {
    if (Update.LoadConfigBytes.size() != 8)
      return reject("SafeSEH update has an invalid field extent");
    auto Offset = rawOffset(
        PE, Binary,
        Update.LoadConfigRVA +
            offsetof(llvm::object::coff_load_configuration32, SEHandlerTable),
        8);
    if (!Offset)
      return Offset.takeError();
    std::copy(Update.LoadConfigBytes.begin(), Update.LoadConfigBytes.end(),
              Binary.begin() + *Offset);
  }
  setPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE,
                     Update.RelocationRVA, Update.RelocationSize);
  clearPEChecksum(PE);
  return llvm::Error::success();
#endif
}

llvm::Error
validateCOFFRegistrationPatch(llvm::ArrayRef<uint8_t> Binary,
                              const COFFRegistrationPatchUpdate &Update) {
  if (!Update.Apply)
    return llvm::Error::success();
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return reject("LLVM does not provide indexed x86 SEH output receipts");
#else
  auto PE =
      locatePEHeaders(const_cast<uint8_t *>(Binary.data()), Binary.size());
  if (!PE.valid() || PE.Is64 ||
      PE.FileHeader->Machine != llvm::COFF::IMAGE_FILE_MACHINE_I386 ||
      getPEImageBase(PE) != Update.ImageBase ||
      getPE32OptionalHeader(PE)->DLLCharacteristics !=
          Update.DllCharacteristics ||
      PE.FileHeader->Characteristics != Update.FileCharacteristics)
    return reject("final registration image changed its PE32 safety flags");
  auto Offset = rawOffset(PE, Binary, Update.SectionRVA, Update.SectionSize);
  if (!Offset)
    return Offset.takeError();
  if (llvm::SHA256::hash(Binary.slice(*Offset, Update.SectionSize)) !=
      Update.SectionSHA256)
    return reject("installed registration image differs from the validated "
                  "compiler bytes");
  bool Executable = false;
  forEachPESection(PE, [&](const PESectionFields &S, uint16_t) {
    if (Update.SectionRVA >= S.VirtualAddress &&
        rangeInBounds(uint64_t(Update.SectionRVA) - S.VirtualAddress,
                      Update.SectionSize,
                      getPESectionContentSize(S.VirtualSize, S.SizeOfRawData)))
      Executable = (S.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) &&
                   (S.Characteristics & llvm::COFF::IMAGE_SCN_MEM_READ) &&
                   !(S.Characteristics & llvm::COFF::IMAGE_SCN_MEM_WRITE);
  });
  if (!Executable)
    return reject(
        "installed registration section has incompatible memory permissions");
  if (Update.PatchedEntryRVAs.empty() ||
      Update.EntryEncodings.size() != Update.PatchedEntryRVAs.size() ||
      Update.PatchedEntryRVAs.size() > limits::kMaxRegistrationEHStateWork)
    return reject("installed registration has no bounded entry receipt");
  uint64_t PreviousEnd = 0;
  for (const auto &[Original, Generated] : Update.PatchedEntryRVAs) {
    if (Original < PreviousEnd ||
        uint64_t(Original) + Update.ImageBase + 5 > uint64_t(UINT32_MAX) + 1 ||
        Generated < Update.SectionRVA ||
        uint64_t(Generated) - Update.SectionRVA >= Update.SectionSize)
      return reject("installed registration entry receipt has invalid extents");
    PreviousEnd = uint64_t(Original) + 5;
    auto Entry = rawOffset(PE, Binary, Original, 5);
    if (!Entry)
      return Entry.takeError();
    size_t Owners = 0;
    forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
      if (Original >= Section.VirtualAddress &&
          rangeInBounds(uint64_t(Original) - Section.VirtualAddress, 5,
                        getPESectionContentSize(Section.VirtualSize,
                                                Section.SizeOfRawData)) &&
          (Section.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) &&
          (Section.Characteristics & llvm::COFF::IMAGE_SCN_MEM_READ))
        ++Owners;
    });
    const auto *Bytes = Binary.data() + *Entry;
    if (Owners != 1 || Bytes[0] != 0xe9 ||
        uint32_t(Original + 5 + readLE<uint32_t>(Bytes + 1)) != Generated)
      return reject("installed registration entry differs from its exact "
                    "compiler trampoline");
  }
  size_t CxxOwners = 0;
  for (const auto &Encoding : Update.EntryEncodings) {
    if (!Encoding)
      continue;
    if (*Encoding == ExceptionEncoding::X86CxxFuncInfo)
      ++CxxOwners;
    else if (*Encoding != ExceptionEncoding::X86ScopeTableEH3 &&
             *Encoding != ExceptionEncoding::X86ScopeTableEH4)
      return reject("installed registration has an unsupported entry encoding");
  }
  if (CxxOwners != Update.GeneratedCxxGraphs.size())
    return reject("installed C++ graph set differs from its entry encodings");
  if (CxxOwners) {
    BinaryImage Reparsed;
    Reparsed.Base = Update.ImageBase;
    Reparsed.Arch = Arch::X86;
    Reparsed.Bits = Bitness::Bits32;
    Reparsed.Format = BinaryFormat::COFF;
    bool InvalidSections = false;
    std::vector<std::pair<uint64_t, uint64_t>> VirtualRanges, RawRanges;
    forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
      const uint64_t Extent = std::max(uint32_t(Section.VirtualSize),
                                       uint32_t(Section.SizeOfRawData));
      if (!Extent)
        return;
      const uint64_t VA = uint64_t(Update.ImageBase) + Section.VirtualAddress;
      if (VA > UINT32_MAX || Extent > uint64_t(UINT32_MAX) + 1 - VA ||
          !rangeInBounds(Section.PointerToRawData, Section.SizeOfRawData,
                         Binary.size())) {
        InvalidSections = true;
        return;
      }
      VirtualRanges.emplace_back(VA, VA + Extent);
      if (Section.SizeOfRawData)
        RawRanges.emplace_back(Section.PointerToRawData,
                               uint64_t(Section.PointerToRawData) +
                                   Section.SizeOfRawData);
      Segment Mapped;
      Mapped.VA = VA;
      Mapped.Size = Extent;
      Mapped.Flags = coffFlagsToNd(Section.Characteristics);
      Mapped.Data.assign(Binary.begin() + Section.PointerToRawData,
                         Binary.begin() + Section.PointerToRawData +
                             Section.SizeOfRawData);
      Reparsed.Segments.push_back(std::move(Mapped));
    });
    auto Overlaps = [](auto &Ranges) {
      llvm::sort(Ranges);
      for (size_t I = 1; I < Ranges.size(); ++I)
        if (Ranges[I].first < Ranges[I - 1].second)
          return true;
      return false;
    };
    if (InvalidSections || Overlaps(VirtualRanges) || Overlaps(RawRanges))
      return reject("installed C++ graph has conflicting PE section storage");
    std::set<va_t> FuncInfos;
    size_t Work = CxxOwners;
    for (const auto &Expected : Update.GeneratedCxxGraphs) {
      const auto Charge = [&](size_t Amount) {
        if (Amount > limits::kMaxRegistrationEHStateWork - Work)
          return false;
        Work += Amount;
        return true;
      };
      if (!Charge(Expected.UnwindMap.size() + Expected.TryBlocks.size() +
                  Expected.IPMap.size() + Expected.ExceptionSpecTypes.size() +
                  1))
        return reject(
            "installed C++ graph reanalysis exhausted its work budget");
      for (const auto &Try : Expected.TryBlocks)
        if (!Charge(Try.Handlers.size()))
          return reject(
              "installed C++ catch reanalysis exhausted its work budget");
      if (!FuncInfos.insert(Expected.NativeFuncInfoVA).second)
        return reject("installed C++ graphs reuse one FuncInfo owner");
      auto Decoded = coff_loader::getCheckedX86CxxFuncInfoRecords(
          Reparsed, Expected.NativeFuncInfoVA);
      if (!Decoded || !Decoded->HasDistinctRanges || Decoded->Cxx != Expected)
        return reject(
            "installed C++ FuncInfo differs from its normalized graph");
      for (const auto &Range : Decoded->Ranges)
        if (Range.Begin < uint64_t(Update.ImageBase) + Update.SectionRVA ||
            Range.End > uint64_t(Update.ImageBase) + Update.SectionRVA +
                            Update.SectionSize)
          return reject("installed C++ record leaves its generated section");
    }
  }
  const auto *Reloc = getPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE);
  if (!Reloc || Reloc->RelativeVirtualAddress != Update.RelocationRVA ||
      Reloc->Size != Update.RelocationSize)
    return reject(
        "installed relocation directory differs from the prepared contract");
  const auto *Config = getPEDataDirectory(PE, llvm::COFF::LOAD_CONFIG_TABLE);
  if ((Config ? uint32_t(Config->RelativeVirtualAddress) : 0) !=
          Update.LoadConfigRVA ||
      (Config ? uint32_t(Config->Size) : 0) != Update.LoadConfigSize)
    return reject("installed load configuration changed its directory extent");
  if (Update.LoadConfigRVA) {
    auto Header = rawOffset(PE, Binary, Update.LoadConfigRVA, 4);
    if (!Header)
      return Header.takeError();
    if (readLE<uint32_t>(Binary.data() + *Header) !=
        Update.LoadConfigDeclaredSize)
      return reject("installed load configuration changed its declared extent");
  }
  if (!Update.LoadConfigBytes.empty()) {
    if (!Config ||
        Update.LoadConfigDeclaredSize <
            offsetof(llvm::object::coff_load_configuration32, SEHandlerCount) +
                4)
      return reject("installed SafeSEH load configuration changed");
    auto Fields = rawOffset(
        PE, Binary,
        Update.LoadConfigRVA +
            offsetof(llvm::object::coff_load_configuration32, SEHandlerTable),
        8);
    if (!Fields)
      return Fields.takeError();
    if (Update.LoadConfigBytes.size() != 8 ||
        Binary.slice(*Fields, 8) !=
            llvm::ArrayRef<uint8_t>(Update.LoadConfigBytes))
      return reject(
          "installed SafeSEH fields differ from the prepared contract");
  }
  return llvm::Error::success();
#endif
}

} // namespace neverd
