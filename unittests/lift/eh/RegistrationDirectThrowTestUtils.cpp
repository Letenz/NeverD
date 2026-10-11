//===- RegistrationDirectThrowTestUtils.cpp - CRT throw edits ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify callback boundaries and current object arguments of x86 C++ throws.
//===----------------------------------------------------------------------===//
#include "RegistrationDirectThrowTestUtils.h"

#include "RegistrationInitializerTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {
namespace {
void checkCallbackBoundaries(const MedFunc &Med, const BinaryImage &Image) {
  const auto &Source = *Med.ExceptionMetadata;
  const auto Boundary = Source.Cxx->TryBlocks[0].Handlers[0].HandlerVA + 1;
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Image;
    Changed.ExceptionMetadata.Functions.clear();
    Changed.ExceptionMetadata.rebuildIndex();
    auto BoundarySymbol = Symbol::makeFunc(Boundary);
    BoundarySymbol.IsBoundaryGuess = true;
    if (Mutation == 1) {
      BoundarySymbol.Name = "independent_entry";
      BoundarySymbol.Origin = NameOrigin::Stated;
    }
    Changed.Symbols.push_back(BoundarySymbol);
    if (Mutation == 2) {
      Export Entry;
      Entry.Name = "independent_entry";
      Entry.Addr = Boundary;
      Changed.Exports.push_back(Entry);
    }
    if (Mutation >= 3) {
      auto Segment =
          llvm::find_if(Changed.Segments, [&](const auto &Candidate) {
            return Boundary > Candidate.VA &&
                   Boundary - 1 - Candidate.VA < Candidate.Data.size();
          });
      ASSERT_NE(Segment, Changed.Segments.end());
      // A real return, trap or unresolved jump keeps even a weak boundary.
      const uint8_t Terminators[] = {0xc3, 0xcc, 0xff};
      Segment->Data[Boundary - 1 - Segment->VA] = Terminators[Mutation - 3];
      if (Mutation == 5) {
        ASSERT_LT(Boundary - Segment->VA, Segment->Data.size());
        Segment->Data[Boundary - Segment->VA] = 0xe0; // JMP EAX.
      }
    }
    coff_loader::parseX86RegistrationExceptions(Changed);
    const auto Found = llvm::find_if(
        Changed.ExceptionMetadata.Functions, [&](const auto &Candidate) {
          return Candidate.CodeRange.Begin == Source.CodeRange.Begin;
        });
    ASSERT_NE(Found, Changed.ExceptionMetadata.Functions.end());
    EXPECT_EQ(Found->CodeRange.End,
              Mutation == 0 ? Source.CodeRange.End : Boundary);
  }
}
} // namespace
void checkDirectThrowEdits(const MedFunc &Med, const llvm::Function &Parent,
                           const BinaryImage &Image) {
  checkCallbackBoundaries(Med, Image);
  checkInitializerEdits(Med, Parent, Image);
  for (bool Callback : {false, true})
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(Callback);
      SCOPED_TRACE(Mutation);
      auto Module = llvm::CloneModule(*Parent.getParent());
      auto *Function = Module->getFunction(Parent.getName());
      std::vector<llvm::CallBase *> Calls;
      for (auto &Block : *Function)
        for (auto &I : Block)
          if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
            if (const auto *Target = Call->getCalledFunction();
                Target &&
                Target->getName().starts_with(
                    "__nd_registration_runtime_throw_") &&
                !llvm::isa<llvm::ConstantPointerNull>(Call->getArgOperand(1)))
              Calls.push_back(Call);
      ASSERT_GE(Calls.size(), 3u);
      auto Selected = llvm::find_if(Calls, [&](const auto *Call) {
        return Call->getOperandBundle(llvm::LLVMContext::OB_funclet)
                   .has_value() == Callback;
      });
      if (Selected == Calls.end()) {
        ASSERT_TRUE(Callback);
        continue;
      }
      auto *Call = *Selected;
      llvm::IRBuilder<> Builder(Call);
      if (Mutation == 0)
        Call->setArgOperand(0,
                            llvm::ConstantPointerNull::get(Builder.getPtrTy()));
      if (Mutation == 1 || Mutation == 2) {
        const unsigned Argument = Mutation - 1;
        Call->setArgOperand(Argument,
                            Builder.CreateGEP(Builder.getInt8Ty(),
                                              Call->getArgOperand(Argument),
                                              Builder.getInt32(1)));
      }
      if (Mutation == 3) {
        const auto Other = llvm::find_if(Calls, [&](const auto *Candidate) {
          return Candidate->getArgOperand(1) != Call->getArgOperand(1);
        });
        ASSERT_NE(Other, Calls.end());
        Call->setArgOperand(1, (*Other)->getArgOperand(1));
      }
      if (Mutation == 4)
        Call->addParamAttr(0, llvm::Attribute::InReg);
      if (Mutation == 5)
        Call->getCalledFunction()->addParamAttr(1, llvm::Attribute::NonNull);
      if (Mutation == 6)
        Call->setMemoryEffects(llvm::MemoryEffects::none());
      if (Mutation == 7)
        Call->getCalledFunction()->setMemoryEffects(
            llvm::MemoryEffects::none());
      if (Mutation == 8 || Mutation == 9) {
        // Remove definite initialization of the selected throw object. Scalar
        // fixtures store seven directly; a catch constructor instead consumes
        // the caught object's current fields.
        unsigned Stores = 0;
        llvm::DominatorTree Dominators(*Function);
        for (auto &Block : *Function)
          for (auto It = Block.begin(); It != Block.end();) {
            auto *Store = llvm::dyn_cast<llvm::StoreInst>(&*It++);
            const auto *Value = Store ? llvm::dyn_cast<llvm::ConstantInt>(
                                            Store->getValueOperand())
                                      : nullptr;
            if (!Value || !Dominators.dominates(Store, Call) ||
                !Value->getType()->isIntegerTy(32) ||
                (Value->getZExtValue() != 7 &&
                 Value->getZExtValue() != 0x40e00000))
              continue;
            ++Stores;
            if (Mutation == 8)
              Store->eraseFromParent();
            else
              Store->setOperand(0, llvm::UndefValue::get(Value->getType()));
          }
        if (!Stores) {
          std::vector<llvm::CallBase *> Initializers;
          for (auto &Block : *Function)
            for (auto &I : Block) {
              auto *Candidate = llvm::dyn_cast<llvm::CallBase>(&I);
              const auto *Target =
                  Candidate ? Candidate->getCalledFunction() : nullptr;
              if (!Target || Target->isIntrinsic() ||
                  !Dominators.dominates(Candidate, Call))
                continue;
              auto Address = rewrite_source::getOriginalVA(*Target);
              if (!Address) {
                llvm::consumeError(Address.takeError());
                continue;
              }
              if (llvm::any_of(Med.RegistrationStates->CalleeContracts,
                               [&](const auto &Contract) {
                                 return Contract.isInitializer() &&
                                        *Address == Contract.Target;
                               }))
                Initializers.push_back(Candidate);
            }
          ASSERT_FALSE(Initializers.empty());
          for (auto *Initializer : Initializers) {
            ASSERT_TRUE(Initializer->use_empty());
            ASSERT_GT(Initializer->arg_size(), 1u);
            if (Mutation == 8) {
              if (auto *Invoke =
                      llvm::dyn_cast<llvm::InvokeInst>(Initializer)) {
                Invoke->getUnwindDest()->removePredecessor(Invoke->getParent());
                llvm::IRBuilder<> Before(Invoke);
                Before.CreateBr(Invoke->getNormalDest());
              }
              Initializer->eraseFromParent();
            } else
              Initializer->setArgOperand(
                  1, llvm::UndefValue::get(
                         Initializer->getArgOperand(1)->getType()));
          }
        }
      }
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      auto Error = validateCOFFRegistrationCxxIR(*Function,
                                                 *Med.ExceptionMetadata, Image);
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
  const auto &States = *Med.RegistrationStates;
  const auto Effect = llvm::find_if(States.CallFrameEffects, [](const auto &C) {
    return C.RuntimeThrow && !C.RuntimeThrow->isRethrow();
  });
  ASSERT_NE(Effect, States.CallFrameEffects.end());
  for (unsigned Argument = 0; Argument != 2; ++Argument) {
    SCOPED_TRACE(Argument);
    auto Changed = Med;
    auto Call = llvm::find_if(Changed.CallInfos, [&](const auto &Candidate) {
      return Candidate.TargetAddr == Effect->Target;
    });
    ASSERT_NE(Call, Changed.CallInfos.end());
    ASSERT_EQ(Call->Args.size(), 2u);
    Call->Args[Argument] = MedVar::makeConst(1, 4);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Changed}, Context, "edited-throw", Arch::X86, {},
                              &Image, BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    const auto *Function = Module->getFunction(Changed.Name);
    ASSERT_TRUE(Function);
    EXPECT_FALSE(Function->getMetadata(windows_eh_md::NativeAttachment));
  }
}
} // namespace neverd::registration_test
