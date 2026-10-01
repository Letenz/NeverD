//===- HighFrameStoreForwardingTests.cpp - Frame value semantics
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <limits>
#include <unordered_set>

namespace neverd {
void forwardPrivateFrameLoads(HighFunc &Function, Arch Architecture);
bool forwardBoundPrivateFrameCopies(HighFunc &Function, Arch Architecture);
void simplifyExprSemantics(std::vector<HighStmt> &Statements);
} // namespace neverd

namespace {
using namespace neverd;

constexpr Arch Architectures[] = {Arch::X86, Arch::X64, Arch::ARM,
                                  Arch::AArch64};

ExprPtr parameter(unsigned Index, TypeRef Type, Arch Architecture) {
  MedVar Variable;
  Variable.Kind = MedVar::Param;
  Variable.Id = Index;
  Variable.Size = Type->Size;
  Variable.TheArch = Architecture;
  return HighExpr::makeVar(Variable, Type);
}

ExprPtr temporary(unsigned Index, TypeRef Type, Arch Architecture) {
  auto Result = parameter(Index, Type, Architecture);
  Result->Var.Kind = MedVar::Temp;
  return Result;
}

ExprPtr entryStackPointer(Arch Architecture) {
  const auto &Target = getTargetRegInfo(Architecture);
  auto Result =
      temporary(900, NdType::makeInt(Target.PointerSize, false), Architecture);
  Result->Var.Kind = MedVar::Reg;
  Result->Var.RegOff = Target.StackPointer;
  return Result;
}

ExprPtr frameSlot(Arch Architecture, uint64_t Distance = 16) {
  return HighExpr::makeBinop(
      NdOp::INT_SUB, entryStackPointer(Architecture),
      HighExpr::makeConst(Distance,
                          getTargetRegInfo(Architecture).PointerSize));
}

HighStmt store(ExprPtr Address, ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Store;
  Result.StoreAddr = std::move(Address);
  Result.StoreVal = std::move(Value);
  return Result;
}

HighStmt assign(ExprPtr Destination, ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Assign;
  Result.Dst = std::move(Destination);
  Result.Val = std::move(Value);
  return Result;
}

HighStmt returning(ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Return;
  Result.RetVal = std::move(Value);
  return Result;
}

HighFunc roundTrip(Arch Architecture, TypeRef Type) {
  HighFunc Result;
  Result.Name = "frame_round_trip";
  Result.FrameSize = 64;
  Result.ReturnType = Type;
  Result.Params = {{"arg0", Type}};
  Result.Body = {
      store(frameSlot(Architecture), parameter(0, Type, Architecture)),
      returning(HighExpr::makeLoad(frameSlot(Architecture), Type))};
  return Result;
}

bool hasLoad(const ExprPtr &Root) {
  std::vector<ExprPtr> Pending{Root};
  std::unordered_set<const HighExpr *> Seen;
  while (!Pending.empty()) {
    auto Expression = Pending.back();
    Pending.pop_back();
    if (!Expression || !Seen.insert(Expression.get()).second)
      continue;
    if (Expression->Kind == ExprKind::Load)
      return true;
    Expression->forEachChildExpr(
        [&](const ExprPtr &Child) { Pending.push_back(Child); });
  }
  return false;
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-value", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-value", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-value", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined",
        "-fsanitize-trap=undefined",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    ASSERT_EQ(Result, 0) << Error << '\n' << Source;
  }
}

TEST(HighFrameStoreForwarding, ForwardsExactIntegerReadsAcrossArchitectures) {
  for (Arch Architecture : Architectures)
    for (uint16_t Bytes : {1, 2, 4, 8})
      for (bool Signed : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Signed);
        auto Function = roundTrip(Architecture, NdType::makeInt(Bytes, Signed));
        forwardPrivateFrameLoads(Function, Architecture);
        EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
        ASSERT_TRUE(Function.Body.back().RetVal->Type);
        EXPECT_EQ(Function.Body.back().RetVal->Type->Size, Bytes);
        EXPECT_EQ(Function.Body.back().RetVal->Type->IsSigned, Signed);
      }
}

TEST(HighFrameStoreForwarding, AcceptsCommutedFrameAdditionOnly) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Bytes = getTargetRegInfo(Architecture).PointerSize;
    const auto NegativeOffset =
        Bytes == 4 ? uint64_t(uint32_t(-16)) : uint64_t(int64_t(-16));
    const auto Commuted = [&] {
      return HighExpr::makeBinop(NdOp::INT_ADD,
                                 HighExpr::makeConst(NegativeOffset, Bytes),
                                 entryStackPointer(Architecture));
    };
    for (bool CommutedStore : {false, true}) {
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      if (CommutedStore)
        Function.Body.front().StoreAddr = Commuted();
      else
        Function.Body.back().RetVal->Operands[0] = Commuted();
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
    }
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    Function.Body.back().RetVal->Operands[0] =
        HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeConst(16, Bytes),
                            entryStackPointer(Architecture));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, PreservesFloatingOperatorsAroundIntegerReads) {
  for (Arch Architecture : Architectures)
    for (bool FusedMultiplyAdd : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(FusedMultiplyAdd);
      const auto Integer = NdType::makeInt(8, false);
      const auto Floating = NdType::makeFloat(8);
      HighFunc Function;
      Function.Name = "stored_floating_bits";
      Function.FrameSize = 64;
      Function.ReturnType = Floating;
      std::vector<ExprPtr> Values;
      for (unsigned Index = 0; Index != (FusedMultiplyAdd ? 3u : 1u); ++Index) {
        const auto Address = frameSlot(Architecture, 16 + 8 * Index);
        Function.Params.push_back({"arg" + std::to_string(Index), Integer});
        Function.Body.push_back(
            store(Address, parameter(Index, Integer, Architecture)));
        Values.push_back(HighExpr::makeBitCast(
            HighExpr::makeLoad(Address, Integer), Floating));
      }
      ExprPtr Result = Values[0];
      if (FusedMultiplyAdd) {
        Result = std::make_shared<HighExpr>();
        Result->Kind = ExprKind::BinOp;
        Result->Op = NdOp::FLOAT_FMA;
        Result->Type = Floating;
        Result->Operands = Values;
      }
      Function.Body.push_back(returning(Result));
      forwardPrivateFrameLoads(Function, Architecture);
      const auto &After = Function.Body.back().RetVal;
      ASSERT_TRUE(After);
      EXPECT_FALSE(hasLoad(After));
      EXPECT_EQ(After->Type, Floating);
      if (FusedMultiplyAdd) {
        EXPECT_EQ(After->Kind, ExprKind::BinOp);
        EXPECT_EQ(After->Op, NdOp::FLOAT_FMA);
        ASSERT_EQ(After->Operands.size(), 3u);
        for (const auto &Operand : After->Operands) {
          EXPECT_EQ(Operand->Kind, ExprKind::BitCast);
          EXPECT_EQ(Operand->Type, Floating);
        }
      } else {
        EXPECT_EQ(After->Kind, ExprKind::BitCast);
      }
    }
}

TEST(HighFrameStoreForwarding, RejectsMalformedOpaqueOperatorArities) {
  for (Arch Architecture : Architectures)
    for (unsigned Operands : {2u, 4u}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Operands);
      const auto Integer = NdType::makeInt(8, false);
      const auto Floating = NdType::makeFloat(8);
      auto Function = roundTrip(Architecture, Integer);
      auto Invalid = std::make_shared<HighExpr>();
      Invalid->Kind = ExprKind::BinOp;
      Invalid->Op = NdOp::FLOAT_FMA;
      Invalid->Type = Floating;
      for (unsigned Index = 0; Index != Operands; ++Index)
        Invalid->Operands.push_back(HighExpr::makeBitCast(
            HighExpr::makeLoad(frameSlot(Architecture), Integer), Floating));
      Function.ReturnType = Floating;
      Function.Body.back().RetVal = Invalid;
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, FollowsDominatingFrameAliases) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Type = NdType::makeInt(4, false);
    auto Function = roundTrip(Architecture, Type);
    auto Alias = temporary(
        12, NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false),
        Architecture);
    Function.Body.insert(Function.Body.begin(),
                         assign(Alias, frameSlot(Architecture)));
    Function.Body[1].StoreAddr = Alias;
    Function.Body[2].RetVal->Operands[0] = Alias;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, LearnsFrameAliasesAfterEarlierSpills) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Type = NdType::makeInt(4, false);
    const auto PointerType =
        NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false);
    auto Function = roundTrip(Architecture, Type);
    auto Alias = temporary(12, PointerType, Architecture);
    Function.Body.insert(
        Function.Body.begin(),
        store(frameSlot(Architecture, 32), parameter(1, Type, Architecture)));
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(Alias, frameSlot(Architecture)));
    Function.Body[2].StoreAddr = Alias;
    Function.Body.back().RetVal->Operands[0] = Alias;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, DoesNotUseLaterAliasForEarlierReads) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Type = NdType::makeInt(4, false);
    const auto PointerType =
        NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false);
    auto Function = roundTrip(Architecture, Type);
    auto Alias = temporary(12, PointerType, Architecture);
    auto Earlier = temporary(13, Type, Architecture);
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(Earlier, HighExpr::makeLoad(Alias, Type)));
    Function.Body.insert(Function.Body.begin() + 2,
                         assign(Alias, frameSlot(Architecture)));
    Function.Body.back().RetVal = Earlier;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body[1].Val));
  }
}

TEST(HighFrameStoreForwarding, RequiresExactTargetWidthForAddressViews) {
  for (Arch Architecture : Architectures)
    for (bool Reinterpret : {false, true})
      for (bool SameWidth : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Reinterpret);
        SCOPED_TRACE(SameWidth);
        const auto Bytes = getTargetRegInfo(Architecture).PointerSize;
        auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
        auto View = [&](ExprPtr Address) {
          if (!SameWidth)
            Address->Type = NdType::makeInt(Bytes == 4 ? 8 : 4, true);
          auto Result = std::make_shared<HighExpr>();
          Result->Kind = Reinterpret ? ExprKind::BitCast : ExprKind::Cast;
          Result->Type = Result->CastTo = NdType::makeInt(Bytes, false);
          Result->Operands = {std::move(Address)};
          return Result;
        };
        Function.Body[0].StoreAddr = View(frameSlot(Architecture));
        Function.Body.back().RetVal->Operands[0] =
            View(frameSlot(Architecture));
        forwardPrivateFrameLoads(Function, Architecture);
        EXPECT_EQ(hasLoad(Function.Body.back().RetVal), !SameWidth);
      }
}

TEST(HighFrameStoreForwarding, PreservesNarrowStoredComplementsWhenExecuted) {
  std::vector<HighFunc> Functions;
  std::string Main = "int main(void) {\n";
  unsigned Index = 0;
  for (uint16_t Bytes : {1, 2})
    for (bool Signed : {false, true})
      for (bool Spilled : {false, true}) {
        const auto Type = NdType::makeInt(Bytes, Signed);
        auto Function = roundTrip(Arch::X64, Type);
        Function.Name = "stored_complement" + std::to_string(Index++);
        Function.ReturnType = NdType::makeInt(8, Signed);
        auto Value =
            HighExpr::makeUnary(NdOp::INT_NOT, parameter(0, Type, Arch::X64));
        Value->Type = Type;
        if (Spilled) {
          Function.Body[0].StoreVal = Value;
          forwardPrivateFrameLoads(Function, Arch::X64);
        } else {
          Function.FrameSize = 0;
          Function.Body = {returning(Value)};
        }
        simplifyExprSemantics(Function.Body);
        ASSERT_FALSE(hasLoad(Function.Body.back().RetVal));
        const std::string NativeType =
            (Signed ? "int" : "uint") + std::to_string(Bytes * 8) + "_t";
        Main += "for (uint32_t x = 0; x < 65536; ++x) {\n";
        Main += "if (" + Function.Name + "((" + NativeType + ")x) != (" +
                NativeType + ")(~(" + NativeType + ")x)) return " +
                std::to_string(Index) + "; }\n";
        Functions.push_back(std::move(Function));
      }
  Main += "return 0; }\n";
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  compileAndRun(Source + Main);
}

TEST(HighFrameStoreForwarding, InvalidatesWritesToTheSameEmittedLocal) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Before = temporary(10, Function.ReturnType, Architecture);
    Before->Var.RenameTag = 3;
    Before->Var.SSAVer = 1;
    auto After = temporary(11, Function.ReturnType, Architecture);
    After->Var.RenameTag = 3;
    After->Var.SSAVer = 2;
    Function.Body[0].StoreVal = Before;
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(After, HighExpr::makeConst(7, 4)));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, DistinguishesUnrenamedVariableKinds) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Temp = temporary(10, Function.ReturnType, Architecture);
    auto Reg = temporary(10, Function.ReturnType, Architecture);
    Reg->Var.Kind = MedVar::Reg;
    Reg->Var.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
    Function.Body[0].StoreVal = Temp;
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(Reg, HighExpr::makeConst(7, 4)));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_FALSE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, InvalidatesWritesThroughStackVariables) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Home = temporary(10, Function.ReturnType, Architecture);
    Home->Var.Kind = MedVar::Stack;
    Home->Var.StackOff = -16;
    Function.Body.insert(Function.Body.begin() + 1,
                         assign(Home, HighExpr::makeConst(7, 4)));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, AliasDefinitionsUseTheEmittedLocalIdentity) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto PointerType =
        NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false);
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Before = temporary(10, PointerType, Architecture);
    Before->Var.RenameTag = 3;
    Before->Var.SSAVer = 1;
    auto After = temporary(11, PointerType, Architecture);
    After->Var.RenameTag = 3;
    After->Var.SSAVer = 2;
    Function.Body.insert(Function.Body.begin(),
                         assign(Before, frameSlot(Architecture)));
    Function.Body[1].StoreAddr = Before;
    Function.Body.insert(Function.Body.begin() + 2,
                         assign(After, frameSlot(Architecture, 32)));
    Function.Body.back().RetVal->Operands[0] = Before;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, StackHomesCannotBecomeImmutableAddressAliases) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto PointerType =
        NdType::makeInt(getTargetRegInfo(Architecture).PointerSize, false);
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Home = temporary(10, PointerType, Architecture);
    Home->Var.Kind = MedVar::Stack;
    Home->Var.StackOff = -8;
    Function.Body.insert(Function.Body.begin(),
                         assign(Home, frameSlot(Architecture)));
    Function.Body.insert(
        Function.Body.begin() + 1,
        store(frameSlot(Architecture, 8), frameSlot(Architecture, 32)));
    Function.Body[2].StoreAddr = Home;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding,
     DoesNotReplaceLoadSignednessWithStoreSignedness) {
  for (Arch Architecture : Architectures)
    for (uint16_t Bytes : {1, 2, 4, 8})
      for (bool Signed : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Signed);
        auto Function = roundTrip(Architecture, NdType::makeInt(Bytes, Signed));
        Function.Body.back().RetVal->Type = NdType::makeInt(Bytes, !Signed);
        forwardPrivateFrameLoads(Function, Architecture);
        // Keeping the read is safe until a byte-accurate reinterpretation is
        // proven; simply reusing the stored signed value is not.
        EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
      }
}

TEST(HighFrameStoreForwarding, InvalidatesUnknownAndOverlappingWrites) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(8, false));
      ExprPtr Address =
          Variant == 0
              ? parameter(1, frameSlot(Architecture)->Type, Architecture)
              : frameSlot(Architecture, Variant == 1 ? 12 : 8);
      Function.Body.insert(Function.Body.begin() + 1,
                           store(Address, HighExpr::makeConst(7, 4)));
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_EQ(hasLoad(Function.Body.back().RetVal), Variant != 2);
    }
}

TEST(HighFrameStoreForwarding, DoesNotInventTheWidthOfAnAssignmentStore) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(8, false));
    Function.Body[0] = assign(
        HighExpr::makeLoad(frameSlot(Architecture), NdType::makeInt(1, false)),
        parameter(0, NdType::makeInt(8, false), Architecture));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, RetainsUnprovenAccessesAndFrameBounds) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 8; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(8, false));
      if (Variant == 0)
        Function.Body.back().RetVal->Type = NdType::makeInt(4, false);
      if (Variant == 1)
        Function.Body.back().RetVal->MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
      if (Variant == 2)
        Function.Body[0].MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
      if (Variant == 3)
        Function.FrameSize = 0;
      if (Variant == 4)
        Function.FrameSize = std::numeric_limits<int64_t>::min();
      if (Variant >= 5) {
        const uint64_t Distance = Variant == 5 ? 4 : Variant == 6 ? 0 : 72;
        Function.Body[0].StoreAddr = frameSlot(Architecture, Distance);
        Function.Body.back().RetVal->Operands[0] =
            frameSlot(Architecture, Distance);
      }
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, KeepsEffectsAndMalformedValuesOutOfSnapshots) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 10; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      const auto Type = NdType::makeInt(4, false);
      auto Function = roundTrip(Architecture, Type);
      ExprPtr Value =
          HighExpr::makeBinop(NdOp::INT_ADD, parameter(0, Type, Architecture),
                              HighExpr::makeConst(1, 4));
      switch (Variant) {
      case 0:
        Value->Op = NdOp::ATOMIC_ADD;
        break;
      case 1:
        Value->Op = NdOp::ATOMIC_XCHG;
        break;
      case 2:
        Value->Op = NdOp::INT_DIV;
        break;
      case 3:
        Value->Operands[1] = nullptr;
        break;
      case 4:
        Value->Operands.pop_back();
        break;
      case 5:
        Value->Type = nullptr;
        break;
      case 6:
        Value->IndirectTarget = HighExpr::makeConst(7, 4);
        break;
      case 7:
        Value = HighExpr::makeUndef(4);
        break;
      case 8:
        Value = HighExpr::makeLoad(frameSlot(Architecture, 8), Type);
        break;
      case 9:
        Value = HighExpr::makeCall("effect", 0x2000, {});
        Value->Type = Type;
        break;
      }
      Function.Body[0].StoreVal = Value;
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, DoesNotEraseMalformedMemoryExpressions) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 4; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      if (Variant == 3) {
        auto Address = frameSlot(Architecture);
        // A synthetic entry-SP leaf with children is not a valid address.
        Address->Operands[0]->Operands.push_back(HighExpr::makeConst(7, 4));
        Function.Body[0].StoreAddr = Address;
        Function.Body.back().RetVal->Operands[0] = Address;
      } else {
        auto Invalid =
            HighExpr::makeLoad(frameSlot(Architecture, 8), Function.ReturnType);
        if (Variant == 0)
          Invalid->Operands.clear();
        if (Variant == 1)
          Invalid->Operands.push_back(HighExpr::makeConst(7, 4));
        if (Variant == 2)
          Invalid->Type = nullptr;
        Function.Body.back().RetVal = HighExpr::makeBinop(
            NdOp::INT_ADD, Function.Body.back().RetVal, Invalid);
      }
      forwardPrivateFrameLoads(Function, Architecture);
      auto Observed = Function.Body.back().RetVal;
      if (Variant != 3)
        Observed = Observed->Operands[0];
      EXPECT_EQ(Observed->Kind, ExprKind::Load);
    }
}

TEST(HighFrameStoreForwarding, DoesNotCarryFactsAcrossBranchesOrCalls) {
  for (Arch Architecture : Architectures)
    for (bool Call : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      HighStmt Barrier;
      if (Call) {
        Barrier.Kind = StmtKind::ExprStmt;
        Barrier.Val = HighExpr::makeCall("effect", 0x2000, {});
      } else {
        Barrier.Kind = StmtKind::If;
        Barrier.Cond = parameter(1, Function.ReturnType, Architecture);
        Barrier.Body = {
            store(frameSlot(Architecture), HighExpr::makeConst(7, 4))};
      }
      Function.Body.insert(Function.Body.begin() + 1, std::move(Barrier));
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, RejectsWritesToTheSyntheticEntryStackPointer) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    Function.Body.insert(
        Function.Body.begin() + 1,
        assign(entryStackPointer(Architecture), frameSlot(Architecture, 32)));
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, BoundsRenderedExpansionOfSharedDags) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Value = parameter(0, Function.ReturnType, Architecture);
    // Only 21 distinct nodes, but over two million rendered occurrences.
    for (unsigned Level = 0; Level != 20; ++Level)
      Value = HighExpr::makeBinop(NdOp::INT_ADD, Value, Value);
    Function.Body[0].StoreVal = Value;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, DoesNotForwardCyclicValues) {
  for (Arch Architecture : Architectures) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
    auto Value = HighExpr::makeBinop(
        NdOp::INT_ADD, parameter(0, Function.ReturnType, Architecture),
        HighExpr::makeConst(1, 4));
    Value->Operands[1] = Value;
    Function.Body[0].StoreVal = Value;
    forwardPrivateFrameLoads(Function, Architecture);
    EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    Value->Operands[1] = nullptr;
  }
}

ExprPtr integerView(ExprPtr Value, TypeRef Type) {
  auto Result = std::make_shared<HighExpr>();
  Result->Kind = ExprKind::Cast;
  Result->Type = Result->CastTo = std::move(Type);
  Result->Operands = {std::move(Value)};
  return Result;
}

HighStmt boundCall(Arch Architecture, ExprPtr Argument) {
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  Hint->Signature.ReturnType = NdType::makeVoid();
  Hint->Signature.Parameters = {{"value", Argument->Type}};
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error))
      << Error;
  HighStmt Result;
  Result.Kind = StmtKind::Call;
  Result.CallExpr = HighExpr::makeCall("observe", 0x2000, {Argument});
  Result.CallExpr->Type = NdType::makeVoid();
  Result.CallExpr->SourceCallHint = std::move(Hint);
  return Result;
}

std::string statementText(const HighFunc &Function) {
  std::string Result;
  for (const auto &S : Function.Body)
    Result += S.str();
  return Result;
}

TEST(HighBoundPrivateFrameCopies, ExecutesSnapshotsAcrossBranchesAndSlotReuse) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    std::vector<HighFunc> Functions;
    std::string Checks;
    for (uint16_t Bytes : {1, 2, 4, 8}) {
      const auto Type = NdType::makeInt(Bytes, false);
      HighFunc F;
      F.Name = "reused_frame_" + std::to_string(Bytes);
      F.FrameSize = 64;
      F.ReturnType = Type;
      F.Params = {{"choose", NdType::makeInt(8, false)},
                  {"first", Type},
                  {"second", Type}};
      HighStmt Branch;
      Branch.Kind = StmtKind::IfElse;
      Branch.Cond = parameter(0, F.Params[0].Type, Architecture);
      Branch.Body = {assign(temporary(1, Type, Architecture),
                            temporary(0, Type, Architecture))};
      Branch.ElseBody = {assign(temporary(1, Type, Architecture),
                                integerView(temporary(0, Type, Architecture),
                                            NdType::makeInt(Bytes, true)))};
      F.Body = {
          store(frameSlot(Architecture), parameter(1, Type, Architecture)),
          assign(temporary(0, Type, Architecture),
                 HighExpr::makeLoad(frameSlot(Architecture), Type)),
          Branch,
          store(frameSlot(Architecture), parameter(2, Type, Architecture)),
          returning(HighExpr::makeBinop(
              NdOp::INT_XOR, temporary(1, Type, Architecture),
              HighExpr::makeLoad(frameSlot(Architecture), Type)))};
      ASSERT_TRUE(forwardBoundPrivateFrameCopies(F, Architecture));
      eliminateHighDeadPhiCopies(F);
      const auto Report = analyzeHighSourceFlow(F, true);
      ASSERT_TRUE(Report.Complete);
      ASSERT_TRUE(Report.Items.empty());
      walkStmts(F.Body, [&](const HighStmt &S) {
        EXPECT_NE(S.Kind, StmtKind::Store);
        forEachRhsExpr(S, [&](const ExprPtr &E) { EXPECT_FALSE(hasLoad(E)); });
      });
      const auto Ty = "uint" + std::to_string(Bytes * 8) + "_t";
      Checks += "if (" + F.Name + "(i & 1, (" + Ty + ")a, (" + Ty + ")b) != (" +
                Ty + ")(a ^ b)) return " + std::to_string(Bytes) + ";\n";
      Functions.push_back(std::move(F));
    }
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    Options.EmitComments = false;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    compileAndRun(Source + R"(
int main(void) {
  uint64_t a = 0, b = UINT64_MAX;
  for (unsigned i = 0; i < 4096; ++i) {
)" + Checks + R"(
    a = a * UINT64_C(6364136223846793005) + 1;
    b = (b << 7) | (b >> 57);
    b ^= a;
  }
  return 0;
}
)");
  }
}

TEST(HighBoundPrivateFrameCopies, IntersectsAllReachingStores) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Variant = 0; Variant != 4; ++Variant) {
      SCOPED_TRACE(Variant);
      const auto Type = NdType::makeInt(8, false);
      auto F = roundTrip(A, Type);
      F.Params.push_back({"other", Type});
      HighStmt Branch;
      Branch.Kind = StmtKind::IfElse;
      Branch.Cond = parameter(0, Type, A);
      Branch.Body = {F.Body[0]};
      Branch.ElseBody = {F.Body[0]};
      if (Variant == 1)
        Branch.ElseBody.clear();
      if (Variant == 2)
        Branch.ElseBody[0].StoreVal = parameter(1, Type, A);
      if (Variant == 3)
        Branch.ElseBody[0].StoreVal =
            integerView(parameter(0, Type, A), NdType::makeInt(8, true));
      F.Body[0] = Branch;
      const auto Before = statementText(F);
      const bool Expected = Variant == 0 || Variant == 3;
      EXPECT_EQ(forwardBoundPrivateFrameCopies(F, A), Expected);
      EXPECT_EQ(hasLoad(F.Body.back().RetVal), !Expected);
      if (!Expected)
        EXPECT_EQ(statementText(F), Before);
    }
}

TEST(HighBoundPrivateFrameCopies, KeepsFactsSeparateAcrossGuardContexts) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    const auto T = NdType::makeInt(8, false);
    auto F = roundTrip(A, T);
    F.Params.push_back({"other", T});
    HighStmt Store;
    Store.Kind = StmtKind::IfElse;
    Store.Cond = parameter(0, T, A);
    Store.Body = {store(frameSlot(A), HighExpr::makeConst(71, 8))};
    Store.ElseBody = {store(frameSlot(A), HighExpr::makeConst(23, 8))};
    HighStmt Read;
    Read.Kind = StmtKind::IfElse;
    Read.Cond = parameter(0, T, A);
    Read.Body = {F.Body.back()};
    Read.ElseBody = {F.Body.back()};
    F.Body = {Store, Read};
    ASSERT_TRUE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_FALSE(hasLoad(F.Body[1].Body[0].RetVal));
    EXPECT_FALSE(hasLoad(F.Body[1].ElseBody[0].RetVal));
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = A;
    ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
    compileAndRun(Source + R"(
int main(void) {
  return frame_round_trip(0, 0) != 23 || frame_round_trip(1, 0) != 71 ||
         frame_round_trip(UINT64_MAX, 0) != 71;
}
)");
  }
}

TEST(HighBoundPrivateFrameCopies, InvalidatesWritesToTheSameSourceLocal) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (bool Renamed : {false, true}) {
      const auto T = NdType::makeInt(8, false);
      auto F = roundTrip(A, T);
      auto V = temporary(0, T, A);
      auto W = temporary(Renamed ? 1 : 0, T, A);
      if (Renamed)
        V->Var.RenameTag = W->Var.RenameTag = 12;
      F.Body[0].StoreVal = V;
      F.Body.insert(F.Body.begin(), assign(V, parameter(0, T, A)));
      F.Body.insert(F.Body.begin() + 2,
                    assign(parameter(0, T, A), HighExpr::makeConst(99, 8)));
      F.Body.insert(F.Body.begin() + 3, assign(W, HighExpr::makeConst(12, 8)));
      ASSERT_TRUE(forwardBoundPrivateFrameCopies(F, A));
      EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
    }
}

TEST(HighBoundPrivateFrameCopies, RequiresInitializedInvariantFrameAliases) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Variant = 0; Variant != 4; ++Variant) {
      SCOPED_TRACE(Variant);
      const auto T = NdType::makeInt(8, false);
      auto F = roundTrip(A, T);
      auto Alias = temporary(20, T, A);
      HighStmt Define;
      Define.Kind = StmtKind::IfElse;
      Define.Cond = parameter(0, T, A);
      Define.Body = {assign(Alias, frameSlot(A))};
      Define.ElseBody = {assign(Alias, frameSlot(A, Variant == 2 ? 24 : 16))};
      if (Variant == 1)
        Define.ElseBody.clear();
      F.Body[0].StoreAddr = Alias;
      F.Body[1].RetVal = HighExpr::makeLoad(Alias, T);
      F.Body.insert(F.Body.begin() + (Variant == 3 ? 1 : 0), Define);
      const auto Before = statementText(F);
      EXPECT_EQ(forwardBoundPrivateFrameCopies(F, A), Variant == 0);
      if (Variant != 0)
        EXPECT_EQ(statementText(F), Before);
    }
}

TEST(HighBoundPrivateFrameCopies, LaterFrameAssignmentCannotRebindEntryInput) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (bool Renamed : {false, true}) {
      const auto T = NdType::makeInt(8, false);
      auto F = roundTrip(A, T);
      auto Input = parameter(0, T, A);
      auto Destination = Renamed ? temporary(20, T, A) : parameter(0, T, A);
      if (Renamed)
        Input->Var.RenameTag = Destination->Var.RenameTag = 7;
      F.Body = {store(Input, HighExpr::makeConst(7, 8)),
                assign(Destination, frameSlot(A)),
                returning(HighExpr::makeLoad(Destination, T))};
      const auto Before = statementText(F);
      EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
      EXPECT_EQ(statementText(F), Before);
    }
}

TEST(HighBoundPrivateFrameCopies, OrdinaryConversionsDoNotBecomePhiCopies) {
  constexpr auto A = Arch::AArch64;
  const auto T = NdType::makeInt(8, false);
  for (const auto &Destination : {NdType::makeFloat(8), NdType::makeInt(4)}) {
    auto F = roundTrip(A, T);
    F.Body.insert(F.Body.begin(),
                  assign(temporary(20, Destination, A), parameter(0, T, A)));
    ASSERT_TRUE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_FALSE(F.Body[0].IsPhiCopy);
  }
}

TEST(HighBoundPrivateFrameCopies, AliasViewsMustRetainTheFullPointerWidth) {
  constexpr auto A = Arch::AArch64;
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = roundTrip(A, T);
    auto Dst = temporary(20, T, A);
    auto Use = temporary(20, T, A);
    if (Variant == 0 || Variant == 1) {
      Dst->Type = NdType::makeInt(4, false);
      if (Variant == 0)
        Dst->Var.Size = 4;
    } else if (Variant == 2) {
      Use->Type = NdType::makeFloat(8);
    } else {
      Use->Var.Size = 4;
    }
    F.Body[0].StoreAddr = Use;
    F.Body.back().RetVal = HighExpr::makeLoad(Use, T);
    F.Body.insert(F.Body.begin(), assign(Dst, frameSlot(A)));
    const auto Before = statementText(F);
    EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_EQ(statementText(F), Before);
  }
}

TEST(HighBoundPrivateFrameCopies, CallsRequireABIBindingAndNoFrameEscape) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Variant = 0; Variant != 7; ++Variant) {
      SCOPED_TRACE(Variant);
      const auto T = NdType::makeInt(8, false);
      auto F = roundTrip(A, T);
      auto Call = boundCall(A, parameter(0, T, A));
      switch (Variant) {
      case 0:
        break;
      case 1:
        Call.CallExpr->SourceCallHint = nullptr;
        break;
      case 2:
        Call.CallExpr->Operands[0] = frameSlot(A);
        break;
      case 3:
        Call.CallExpr->IndirectTarget = frameSlot(A);
        break;
      case 4:
        Call.CallExpr->Operands.push_back(frameSlot(A));
        break;
      case 5:
        Call.CallExpr->Operands[0] = temporary(20, T, A);
        F.Body.insert(F.Body.begin(),
                      assign(temporary(20, T, A), frameSlot(A)));
        break;
      case 6:
        Call.CallExpr->IntrinsicOutputs = {temporary(2, T, A)->Var};
        break;
      }
      F.Body.insert(F.Body.end() - 1, Call);
      const auto Before = statementText(F);
      EXPECT_EQ(forwardBoundPrivateFrameCopies(F, A), Variant == 0);
      EXPECT_EQ(F.Body[F.Body.size() - 2].CallExpr, Call.CallExpr);
      if (Variant != 0)
        EXPECT_EQ(statementText(F), Before);
    }
}

TEST(HighBoundPrivateFrameCopies, RejectsEscapesEffectsAndIncompleteFlow) {
  for (unsigned Variant = 0; Variant != 15; ++Variant) {
    SCOPED_TRACE(Variant);
    constexpr auto A = Arch::AArch64;
    const auto T = NdType::makeInt(8, false);
    auto F = roundTrip(A, T);
    HighStmt Extra;
    switch (Variant) {
    case 0:
      Extra = store(parameter(0, T, A), frameSlot(A));
      break;
    case 1:
      Extra = returning(frameSlot(A));
      break;
    case 2:
      Extra.Kind = StmtKind::ExprStmt;
      Extra.Val = std::make_shared<HighExpr>();
      Extra.Val->Kind = ExprKind::Addr;
      Extra.Val->Operands = {parameter(0, T, A)};
      break;
    case 3:
      F.Body[0].MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 4:
      F.Body.back().RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 5:
      F.Body.back().RetVal->Op = NdOp::ATOMIC_ADD;
      break;
    case 6:
      Extra = assign(entryStackPointer(A), parameter(0, T, A));
      break;
    case 7:
      Extra.Kind = StmtKind::Goto;
      Extra.GotoTarget = 0x8000;
      break;
    case 8:
      Extra.Kind = StmtKind::While;
      Extra.Cond = parameter(0, T, A);
      break;
    case 9:
      F.Body[0].Addr = 0x8000;
      Extra.Kind = StmtKind::Goto;
      Extra.GotoTarget = 0x8000;
      break;
    case 10:
      F.Body[0].StoreAddr = frameSlot(A, 72);
      break;
    case 11:
      F.Body[0].StoreVal->Operands = {HighExpr::makeConst(12, 8)};
      break;
    case 12:
      F.Body[0].Body = {boundCall(A, parameter(0, T, A))};
      break;
    case 13:
      F.Body.back().RetVal->Operands[0] = nullptr;
      break;
    case 14:
      F.Body[0].CallExpr = boundCall(A, parameter(0, T, A)).CallExpr;
      break;
    }
    F.Body.insert(F.Body.end() - 1, Extra);
    const auto OriginalLoad = F.Body.back().RetVal;
    EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_EQ(F.Body.back().RetVal, OriginalLoad);
  }
}

TEST(HighBoundPrivateFrameCopies, KeepsOverlappingAndMixedWidthSlots) {
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    constexpr auto A = Arch::AArch64;
    const auto T = NdType::makeInt(8, false);
    auto F = roundTrip(A, T);
    const auto Other = Variant == 2
                           ? NdType::makeFloat(8)
                           : NdType::makeInt(Variant == 0 ? 4 : 8, false);
    if (Variant == 3)
      F.Body[0].StoreVal = HighExpr::makeBinop(
          NdOp::INT_ADD, parameter(0, T, A), HighExpr::makeConst(1, 8));
    else
      F.Body.insert(
          F.Body.end() - 1,
          store(frameSlot(A, Variant == 1 ? 20 : 16), temporary(2, Other, A)));
    EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
    EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
  }
}

TEST(HighBoundPrivateFrameCopies, RejectsCyclesAndBudgetExhaustionAtomically) {
  constexpr auto A = Arch::AArch64;
  const auto T = NdType::makeInt(8, false);
  auto F = roundTrip(A, T);
  auto Cyclic = integerView(parameter(0, T, A), T);
  Cyclic->Operands[0] = Cyclic;
  F.Body[0].StoreVal = Cyclic;
  EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
  EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
  Cyclic->Operands[0] = nullptr;
  F = roundTrip(A, T);
  F.Body.insert(F.Body.begin(), 100001, HighStmt{});
  const auto Original = F.Body.back().RetVal;
  EXPECT_FALSE(forwardBoundPrivateFrameCopies(F, A));
  EXPECT_EQ(F.Body.back().RetVal, Original);
}
} // namespace
