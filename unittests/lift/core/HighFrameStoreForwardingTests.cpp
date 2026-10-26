//===- HighFrameStoreForwardingTests.cpp - Frame value semantics ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check frame load forwarding across widths, effects and source boundaries.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace neverd {
void forwardPrivateFrameLoads(HighFunc &Function, Arch Architecture);
bool forwardBoundPrivateFrameCopies(HighFunc &Function, Arch Architecture);
void simplifyExprSemantics(std::vector<HighStmt> &Statements);
bool simplifyAllExprs(std::vector<HighStmt> &Statements);
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

TEST(HighFrameStoreForwarding, EmptyDeadStoreAnchorsPreservePrivateFacts) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      HighStmt Anchor;
      Anchor.Kind = StmtKind::Block;
      Anchor.Addr = 0x401004;
      if (Variant == 1)
        Anchor.Body.push_back(
            store(frameSlot(Architecture), HighExpr::makeConst(7, 4)));
      else if (Variant == 2)
        Anchor.Val = HighExpr::makeConst(7, 4);
      Function.Body.insert(Function.Body.begin() + 1, Anchor);
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_EQ(hasLoad(Function.Body.back().RetVal), Variant != 0);
      EXPECT_EQ(Function.Body[1].Kind, StmtKind::Block);
      EXPECT_EQ(Function.Body[1].Addr, 0x401004U);
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

TEST(HighFrameStoreForwarding, PreservesStoredBitsAcrossSignedIntegerViews) {
  std::vector<HighFunc> Functions;
  std::string Main = R"(
int main(void) {
  const uint64_t boundaries[] = {
    0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff,
    0x7fffffff, 0x80000000, 0xffffffff,
    UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000), UINT64_MAX
  };
)";
  for (Arch Architecture : Architectures)
    for (uint16_t Bytes : {1, 2, 4, 8})
      for (bool Signed : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Signed);
        const auto Stored = NdType::makeInt(Bytes, Signed);
        const auto Read = NdType::makeInt(Bytes, !Signed);
        auto Function = roundTrip(Architecture, Stored);
        Function.ReturnType = Read;
        Function.Body.back().RetVal->Type = Read;
        forwardPrivateFrameLoads(Function, Architecture);
        ASSERT_FALSE(hasLoad(Function.Body.back().RetVal));
        ASSERT_EQ(Function.Body.back().RetVal->Kind, ExprKind::Cast);
        EXPECT_EQ(Function.Body.back().RetVal->Type, Read);
        EXPECT_EQ(Function.Body.back().RetVal->Type->IsSigned, !Signed);
        if (Architecture != Arch::X64)
          continue;
        Function.Name = "stored_view" + std::to_string(Functions.size());
        const std::string Bits = std::to_string(Bytes * 8) + "_t";
        const std::string WriteType = (Signed ? "int" : "uint") + Bits;
        const std::string ReadType = (Signed ? "uint" : "int") + Bits;
        Main += "{ uint64_t x = 1; for (unsigned i = 0; i < 65536; ++i) {\n";
        Main += "x = x * UINT64_C(6364136223846793005) + 1;\n";
        Main += "uint64_t bits = i < sizeof(boundaries) / sizeof(*boundaries)"
                " ? boundaries[i] : x;\n";
        Main += WriteType + " stored = (" + WriteType + ")bits;\n";
        Main += ReadType + " expected; memcpy(&expected, &stored, "
                           "sizeof(expected));\n";
        Main += "if (" + Function.Name + "(stored) != expected) return " +
                std::to_string(Functions.size() + 1) + "; }}\n";
        Functions.push_back(std::move(Function));
      }
  Main += "return 0; }\n";
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  compileAndRun("#include <string.h>\n" + Source + Main);
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

TEST(HighFrameStoreForwarding, ForwardsCallInputsAndClearsFactsAfterTheCall) {
  for (Arch Architecture : Architectures)
    for (StmtKind Kind : {StmtKind::Assign, StmtKind::ExprStmt, StmtKind::Call,
                          StmtKind::Return}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(static_cast<int>(Kind));
      const auto Type = NdType::makeInt(4, false);
      auto Function = roundTrip(Architecture, Type);
      auto Input = HighExpr::makeLoad(frameSlot(Architecture), Type);
      auto Call = HighExpr::makeCall("consume", 0x2000, {Input});
      Call->Type = Type;
      HighStmt Barrier;
      Barrier.Kind = Kind;
      if (Kind == StmtKind::Call)
        Barrier.CallExpr = Call;
      else if (Kind == StmtKind::Return)
        Barrier.RetVal = Call;
      else {
        Barrier.Val = Call;
        if (Kind == StmtKind::Assign)
          Barrier.Dst = temporary(1, Type, Architecture);
      }
      Function.Body.insert(Function.Body.begin() + 1, std::move(Barrier));
      if (Kind == StmtKind::Return)
        Function.Body.pop_back();
      forwardPrivateFrameLoads(Function, Architecture);
      const auto &Observed = Function.Body[1];
      const auto Rebuilt = Kind == StmtKind::Call     ? Observed.CallExpr
                           : Kind == StmtKind::Return ? Observed.RetVal
                                                      : Observed.Val;
      ASSERT_EQ(Rebuilt->Kind, ExprKind::Call);
      ASSERT_EQ(Rebuilt->Operands.size(), 1u);
      EXPECT_FALSE(hasLoad(Rebuilt->Operands[0]));
      EXPECT_EQ(Function.Body.front().Kind, StmtKind::Store);
      if (Kind != StmtKind::Return)
        EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
    }
}

TEST(HighFrameStoreForwarding, KeepsReadsAroundNestedCallsAndIndirectTargets) {
  for (Arch Architecture : Architectures)
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Variant);
      auto Function = roundTrip(Architecture, NdType::makeInt(4, false));
      const auto Read = Function.Body.back().RetVal;
      auto Effect = HighExpr::makeCall("effect", 0x2000, {});
      Effect->Type = Function.ReturnType;
      if (Variant == 0)
        Function.Body.back().RetVal =
            HighExpr::makeBinop(NdOp::INT_ADD, Read, Effect);
      else {
        auto Call = HighExpr::makeCall("consume", 0x3000, {Read});
        if (Variant == 1)
          Call->Operands.push_back(Effect);
        else
          Call->IndirectTarget = Effect;
        Function.Body.back().RetVal = Call;
      }
      forwardPrivateFrameLoads(Function, Architecture);
      EXPECT_TRUE(hasLoad(Function.Body.back().RetVal));
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

TEST(HighFrameStoreForwarding, OptimizesBothSidesOfACallIndependently) {
  for (Arch A : Architectures) {
    auto F = roundTrip(A, NdType::makeInt(4, false));
    auto Before = temporary(20, F.ReturnType, A);
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall(
        "effect", 0x2000, {HighExpr::makeLoad(frameSlot(A), F.ReturnType)});
    const auto SharedCall = Call.CallExpr;
    F.Body = {F.Body.front(),
              assign(Before, HighExpr::makeLoad(frameSlot(A), F.ReturnType)),
              Call, store(frameSlot(A), HighExpr::makeConst(7, 4)),
              F.Body.back()};
    forwardPrivateFrameLoads(F, A);
    EXPECT_FALSE(hasLoad(F.Body[1].Val));
    EXPECT_FALSE(hasLoad(F.Body[2].CallExpr->Operands[0]));
    EXPECT_TRUE(hasLoad(SharedCall->Operands[0]));
    EXPECT_FALSE(hasLoad(F.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, CapturedReadsRemoveOnlyRedundantWritebacks) {
  const auto A = Arch::X64;
  const auto Type = NdType::makeInt(8, false);
  auto F = roundTrip(A, Type);
  auto Captured = temporary(20, Type, A);
  F.Body = {assign(Captured, HighExpr::makeLoad(frameSlot(A), Type)),
            store(frameSlot(A, 24), Captured), store(frameSlot(A), Captured),
            returning(HighExpr::makeLoad(frameSlot(A), Type))};
  forwardPrivateFrameLoads(F, A);
  EXPECT_TRUE(hasLoad(F.Body[0].Val));
  EXPECT_EQ(F.Body[1].Kind, StmtKind::Store);
  EXPECT_EQ(F.Body[2].Kind, StmtKind::Block);
  EXPECT_FALSE(hasLoad(F.Body.back().RetVal));
  // The independent original read is retained; rewriting shared observations
  // never changes when memory was sampled.
  EXPECT_EQ(F.Body[0].Val->Kind, ExprKind::Load);
}

TEST(HighFrameStoreForwarding, CapturedReadFactsExpireOnWritesAndCalls) {
  const auto A = Arch::X64;
  const auto Type = NdType::makeInt(8, false);
  for (bool Call : {false, true}) {
    auto F = roundTrip(A, Type);
    auto Captured = temporary(20, Type, A);
    HighStmt Barrier = assign(Captured, HighExpr::makeConst(99, 8));
    if (Call) {
      Barrier.Kind = StmtKind::Call;
      Barrier.Dst.reset();
      Barrier.Val.reset();
      Barrier.CallExpr = HighExpr::makeCall("effect", 0x2000, {});
    }
    F.Body = {assign(Captured, HighExpr::makeLoad(frameSlot(A), Type)), Barrier,
              store(frameSlot(A), Captured), returning(Captured)};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[2].Kind, StmtKind::Store);
  }
}

TEST(HighFrameStoreForwarding, NestedArgumentEffectsDoNotUseEarlierFacts) {
  auto F = roundTrip(Arch::X64, NdType::makeInt(4, false));
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall(
      "outer", 0x2000,
      {HighExpr::makeCall("mutate", 0x3000, {}),
       HighExpr::makeLoad(frameSlot(Arch::X64), F.ReturnType)});
  F.Body.insert(F.Body.begin() + 1, Call);
  forwardPrivateFrameLoads(F, Arch::X64);
  EXPECT_TRUE(hasLoad(F.Body[1].CallExpr->Operands[1]));
  EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
}

TEST(HighFrameStoreForwarding, BranchesAndLoopBodiesHavePrivateRegions) {
  for (bool Loop : {false, true}) {
    auto F = roundTrip(Arch::X64, NdType::makeInt(4, false));
    HighStmt Region;
    Region.Kind = Loop ? StmtKind::While : StmtKind::If;
    Region.Cond = parameter(1, F.ReturnType, Arch::X64);
    Region.Body = {
        store(frameSlot(Arch::X64), HighExpr::makeConst(7, 4)),
        assign(temporary(20, F.ReturnType, Arch::X64),
               HighExpr::makeLoad(frameSlot(Arch::X64), F.ReturnType))};
    F.Body.insert(F.Body.begin() + 1, Region);
    forwardPrivateFrameLoads(F, Arch::X64);
    EXPECT_FALSE(hasLoad(F.Body[1].Body[1].Val));
    EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
  }
}

TEST(HighFrameStoreForwarding, DoesNotForwardPastAnAlternateEntry) {
  auto F = roundTrip(Arch::X64, NdType::makeInt(4, false));
  F.Body.back().Addr = 0x3000;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x3000;
  F.Body.insert(F.Body.begin(), Jump);
  forwardPrivateFrameLoads(F, Arch::X64);
  EXPECT_TRUE(hasLoad(F.Body.back().RetVal));
}

TEST(HighFrameStoreForwarding, SignExtensionAndBooleanInversionExecuteExactly) {
  std::string Source;
  for (unsigned Bytes : {1u, 2u, 4u}) {
    const auto Type = NdType::makeInt(8, false);
    auto Input = parameter(0, Type, Arch::X64);
    const auto Shift = HighExpr::makeConst(64 - Bytes * 8, 8);
    auto Left = HighExpr::makeBinop(NdOp::INT_LEFT, Input, Shift);
    Left->Type = Type;
    auto Right = HighExpr::makeBinop(NdOp::INT_ASHR, Left, Shift);
    Right->Type = Type;
    auto F = roundTrip(Arch::X64, Type);
    F.Name = "signed_" + std::to_string(Bytes);
    F.FrameSize = 0;
    F.Body = {returning(Right)};
    EXPECT_TRUE(simplifyAllExprs(F.Body));
    EXPECT_EQ(F.Body.back().RetVal->Kind, ExprKind::UnaryOp);
    EXPECT_EQ(F.Body.back().RetVal->Op, NdOp::INT_SEXT);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    Source += Text;
  }
  Source += R"(
int main(void) {
  uint64_t inputs[] = {0,1,127,128,255,256,32767,32768,65535,
                       0x7fffffff,0x80000000,0xffffffff,UINT64_MAX};
  for (unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
    uint64_t x=inputs[i];
    if (signed_1(x)!=(uint64_t)(int64_t)(int8_t)x) return 1;
    if (signed_2(x)!=(uint64_t)(int64_t)(int16_t)x) return 2;
    if (signed_4(x)!=(uint64_t)(int64_t)(int32_t)x) return 3;
  }
  return 0;
})";
  compileAndRun(Source);
}

TEST(HighFrameStoreForwarding, InvertedWidenedPredicateKeepsNarrowSignedTest) {
  const auto T = NdType::makeInt(4, true);
  HighFunc F;
  F.Name = "status_guard";
  F.ReturnType = T;
  F.Params = {{"arg0", T}};
  auto Compare =
      HighExpr::makeBinop(NdOp::INT_SLESSEQUAL, HighExpr::makeConst(0, 4),
                          parameter(0, T, Arch::X64));
  auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Compare);
  Extended->Type = NdType::makeInt(8, false);
  HighStmt Guard;
  Guard.Kind = StmtKind::IfElse;
  Guard.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, Extended, HighExpr::makeConst(0, 8));
  Guard.ElseBody = {returning(HighExpr::makeConst(11, 4))};
  F.Body = {Guard, returning(HighExpr::makeConst(7, 4))};
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  EXPECT_EQ(Source.find("if ((uint64_t)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("arg0 >= 0"), std::string::npos) << Source;
  Source += R"(
int main(void) {
  int32_t inputs[]={INT32_MIN,-1,0,1,INT32_MAX};
  for (unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i)
    if (status_guard(inputs[i])!=(inputs[i]>=0 ? 11 : 7)) return 1;
  return 0;
})";
  compileAndRun(Source);
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

TEST(HighFrameStoreForwarding, OverwritesWithinOneRegionKeepTheFinalWrite) {
  for (const auto A : Architectures) {
    const auto T = NdType::makeInt(4, false);
    for (const bool Same : {false, true}) {
      auto F = roundTrip(A, T);
      auto First = store(frameSlot(A), HighExpr::makeConst(3, 4));
      First.Addr = 0x1000;
      F.Body = {First, store(frameSlot(A, 24), HighExpr::makeConst(8, 4)),
                store(frameSlot(A), HighExpr::makeConst(Same ? 3 : 5, 4)),
                returning(HighExpr::makeLoad(frameSlot(A), T))};
      forwardPrivateFrameLoads(F, A);
      EXPECT_EQ(F.Body[Same ? 2 : 0].Kind, StmtKind::Block);
      EXPECT_EQ(F.Body[Same ? 0 : 2].Kind, StmtKind::Store);
      EXPECT_EQ(F.Body[0].Addr, 0x1000u);
      EXPECT_FALSE(hasLoad(F.Body.back().RetVal));
    }
  }
}

TEST(HighFrameStoreForwarding, OverwriteProofStopsAtObservableBoundaries) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    auto F = roundTrip(A, T);
    HighStmt Between;
    Between.Kind = StmtKind::ExprStmt;
    if (Variant == 0)
      Between.Val = HighExpr::makeLoad(parameter(0, T, A), T);
    else if (Variant == 1)
      Between.Val = HighExpr::makeCall("observe", 0x2000, {frameSlot(A)});
    else if (Variant == 2)
      Between.Val = HighExpr::makeBinop(
          NdOp::INT_DIV, HighExpr::makeConst(7, 8), parameter(0, T, A));
    else {
      Between.Kind = StmtKind::Store;
      Between.StoreAddr = frameSlot(A, 12);
      Between.StoreVal = HighExpr::makeConst(7, 4);
    }
    F.Body.insert(F.Body.begin() + 1, Between);
    F.Body.insert(F.Body.end() - 1,
                  store(frameSlot(A), HighExpr::makeConst(9, 8)));
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[0].Kind, StmtKind::Store) << Variant;
  }
}

TEST(HighFrameStoreForwarding, UnreachableOverwriteCannotDeleteAReachingStore) {
  const auto A = Arch::X64;
  auto F = roundTrip(A, NdType::makeInt(8, false));
  F.Body = {store(frameSlot(A), HighExpr::makeConst(3, 8)),
            returning(frameSlot(A)),
            store(frameSlot(A), HighExpr::makeConst(5, 8))};
  forwardPrivateFrameLoads(F, A);
  EXPECT_EQ(F.Body[0].Kind, StmtKind::Store);
}

TEST(HighFrameStoreForwarding,
     EntryPreservedRegisterSpillReadAsDataStaysDefined) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  auto SP = entryStackPointer(A)->Var;
  auto Saved = SP;
  Saved.Id = 901;
  Saved.RegOff = x86reg::RDI;
  auto Address = temporary(902, T, A)->Var;
  auto Result = temporary(903, T, A)->Var;
  const auto Op = [](NdOp Opcode, MedVar Output,
                     std::initializer_list<MedVar> Inputs) {
    MedOp O;
    O.Opcode = Opcode;
    O.Output = Output;
    for (const auto &Input : Inputs)
      O.addInput(Input);
    return O;
  };
  MedFunc Med;
  Med.Name = "saved_as_data";
  Med.Entry = 0x1000;
  Med.FrameSize = 16;
  Med.CC = CallingConv::Win64;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Med.Entry;
  Block.Ops = {Op(NdOp::COPY, SP, {SP}),
               Op(NdOp::COPY, Saved, {Saved}),
               Op(NdOp::INT_SUB, Address, {SP, MedVar::makeConst(8, 8)}),
               Op(NdOp::STORE, {}, {Address, Saved}),
               Op(NdOp::LOAD, Result, {Address}),
               Op(NdOp::RETURN, {}, {Result})};
  for (size_t I = 0; I < Block.Ops.size(); ++I)
    Block.Ops[I].Addr = Med.Entry + I;
  Med.Blocks = {Block};
  MedToHighConverter Converter;
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    if (Variant) {
      Result = Saved;
      Result.SSAVer = 1;
      Med.Blocks[0].Ops[4].Output = Result;
      Med.Blocks[0].Ops[5].Inputs[0] =
          Variant == 1 ? Result : MedVar::makeConst(0, 8);
    }
    auto High = Converter.convert(Med, A);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = A;
    Options.Format = BinaryFormat::COFF;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    // Its unknown entry value must remain an explicit observation. Erasing
    // only the initializing store leaves an uninitialized C frame read.
    EXPECT_EQ(Source.find("unknown register") != std::string::npos,
              Variant != 2)
        << Source;
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

TEST(HighFrameStoreForwarding, CapturedWritebackCrossesAnOrdinaryBranch) {
  for (const auto A : Architectures) {
    const auto T = NdType::makeInt(8, false);
    auto F = roundTrip(A, T);
    auto Saved = temporary(1, T, A);
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {store(frameSlot(A), Saved)};
    Branch.Body[0].Addr = 0x1040;
    // The source cell is allowed to escape later; the proof concerns the
    // unchanged bits at this writeback, not whole-function frame privacy.
    F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch,
              returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Block);
    EXPECT_EQ(F.Body[1].Body[0].Addr, 0x1040u);
    EXPECT_TRUE(hasLoad(F.Body[0].Val));
  }
}

TEST(HighFrameStoreForwarding, JoinedWritebackNeedsEveryIncomingValue) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const bool Agree : {false, true}) {
    auto F = roundTrip(A, T);
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {store(frameSlot(A), HighExpr::makeConst(3, 8))};
    Branch.ElseBody = {
        store(frameSlot(A), HighExpr::makeConst(Agree ? 3 : 5, 8))};
    F.Body = {Branch, store(frameSlot(A), HighExpr::makeConst(3, 8)),
              returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Kind, Agree ? StmtKind::Block : StmtKind::Store);
  }
}

TEST(HighFrameStoreForwarding, BranchWritebackKeepsClobberedOrChangedBits) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 6; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = roundTrip(A, T);
    auto Saved = temporary(1, T, A);
    HighStmt Effect;
    Effect.Kind = StmtKind::ExprStmt;
    if (Variant == 0)
      Effect.Val = HighExpr::makeCall("modify", 0x2000, {frameSlot(A)});
    else if (Variant == 1)
      Effect = store(parameter(0, T, A), HighExpr::makeConst(5, 8));
    else if (Variant == 2)
      Effect = store(frameSlot(A, 12), HighExpr::makeConst(5, 4));
    else if (Variant == 3)
      Effect = assign(Saved, HighExpr::makeConst(5, 8));
    else if (Variant == 4) {
      Effect.Val = HighExpr::makeLoad(frameSlot(A), T);
      Effect.Val->MemoryOrdering = NdMemoryOrdering::Acquire;
    } else {
      Effect.Val = HighExpr::makeBinop(NdOp::ATOMIC_XCHG, frameSlot(A),
                                       HighExpr::makeConst(5, 8));
    }
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {Effect, store(frameSlot(A), Saved)};
    F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch,
              returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Body[1].Kind, StmtKind::Store);
  }
}

TEST(HighFrameStoreForwarding, FrameCopiesKeepCapturedBitsAcrossSlotReuse) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const bool Replace : {false, true}) {
    auto F = roundTrip(A, T);
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    if (Replace)
      Branch.Body.push_back(store(frameSlot(A, 24), HighExpr::makeConst(7, 8)));
    Branch.Body.push_back(
        store(frameSlot(A), HighExpr::makeLoad(frameSlot(A, 24), T)));
    F.Body = {store(frameSlot(A, 24), HighExpr::makeLoad(frameSlot(A), T)),
              Branch, returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Body.back().Kind,
              Replace ? StmtKind::Store : StmtKind::Block);
  }
}

TEST(HighFrameStoreForwarding, BranchProofPreservesExceptionAndUnwindRegions) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    auto F = roundTrip(A, T);
    auto Saved = temporary(1, T, A);
    HighStmt Region;
    Region.Kind = Variant < 2    ? StmtKind::If
                  : Variant == 2 ? StmtKind::SEHTry
                                 : StmtKind::CxxTry;
    Region.Cond = Variant < 2 ? parameter(0, T, A) : nullptr;
    Region.Body = {store(frameSlot(A), Saved)};
    F.StructuredExceptionRegions = Variant == 0 ? 1 : 0;
    F.UnstructuredExceptionRegions = Variant == 1 ? 1 : 0;
    F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Region,
              returning(frameSlot(A))};
    const auto Original = F.Body[1].Body[0].StoreVal;
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[1].Kind, Region.Kind);
    EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Store);
    EXPECT_EQ(F.Body[1].Body[0].StoreVal, Original);
  }
}

TEST(HighFrameStoreForwarding, BranchWritebackExecutionKeepsCallEffects) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const bool CallBeforeRestore : {false, true}) {
    auto F = roundTrip(A, T);
    F.Name = "writeback_branch";
    F.Params.push_back({"arg1", T});
    auto Saved = temporary(1, T, A);
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall("modify", 0x2000, {frameSlot(A)});
    Call.CallExpr->Type = NdType::makeVoid();
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(1, T, A);
    if (CallBeforeRestore)
      Branch.Body.push_back(Call);
    Branch.Body.push_back(store(frameSlot(A), Saved));
    F.Body = {store(frameSlot(A), parameter(0, T, A)),
              assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch, Call,
              returning(HighExpr::makeLoad(frameSlot(A), T))};
    for (const bool Optimize : {false, true}) {
      auto Output = F;
      if (Optimize)
        forwardPrivateFrameLoads(Output, A);
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      OS << "#include <stdint.h>\nstatic unsigned calls;\n"
            "int modify(uintptr_t address) { uint64_t *p = (uint64_t "
            "*)address; "
            "++calls; *p += 3; return 0; }\n";
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.Format = BinaryFormat::COFF;
      ASSERT_TRUE(HighCEmitter().emit({Output}, OS, Options));
      OS << "\nint main(void) {\n"
            "  if (writeback_branch(17, 0) != 20 || calls != 1) return 1;\n"
            "  if (writeback_branch(23, 1) != 26 || calls != "
         << (CallBeforeRestore ? 3 : 2) << ") return 2;\nreturn 0; }\n";
      compileAndRun(Text);
    }
  }
}

TEST(HighFrameStoreForwarding, BranchAliasesUseTheirReachingFrameOffsets) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (const bool Same : {false, true}) {
    auto F = roundTrip(A, T);
    auto Base = temporary(3, T, A);
    auto Saved = temporary(1, T, A);
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {assign(Base, frameSlot(A, Same ? 16 : 24)),
                   store(Base, Saved)};
    F.Body = {assign(Base, frameSlot(A)),
              assign(Saved, HighExpr::makeLoad(Base, T)), Branch,
              returning(frameSlot(A))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[2].Body[1].Kind, Same ? StmtKind::Block : StmtKind::Store);
  }
}

TEST(HighFrameStoreForwarding, BranchProofDoesNotCommitAcrossCycles) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  auto F = roundTrip(A, T);
  auto Saved = temporary(1, T, A);
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = parameter(0, T, A);
  Branch.Body = {store(frameSlot(A), Saved)};
  HighStmt Again;
  Again.Kind = StmtKind::Goto;
  Again.GotoTarget = 0x1040;
  Branch.Addr = 0x1040;
  F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch, Again};
  forwardPrivateFrameLoads(F, A);
  EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Store);
  EXPECT_EQ(F.Body[2].GotoTarget, 0x1040u);
}

TEST(HighFrameStoreForwarding, BranchProofKeepsExtraStatementEffects) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  auto F = roundTrip(A, T);
  auto Saved = temporary(1, T, A);
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = parameter(0, T, A);
  Branch.Body = {store(frameSlot(A), Saved)};
  Branch.Body[0].Val = HighExpr::makeConst(1, 8);
  F.Body = {assign(Saved, HighExpr::makeLoad(frameSlot(A), T)), Branch,
            returning(frameSlot(A))};
  forwardPrivateFrameLoads(F, A);
  EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Store);
  EXPECT_NE(F.Body[1].Body[0].Val, nullptr);
}
ExprPtr wordDifference(ExprPtr Left, ExprPtr Right, unsigned Form) {
  const auto T = Left->Type;
  if (Form < 2)
    return HighExpr::makeBinop(Form == 0 ? NdOp::INT_SUB : NdOp::INT_XOR, Left,
                               Right);
  auto Negative = [&](ExprPtr V) {
    if (Form == 4)
      return HighExpr::makeUnary(NdOp::INT_NEG2, V);
    auto Factor = HighExpr::makeConst(~uint64_t(0), T->Size,
                                      ConstantAddressProvenance::Scalar);
    Factor->Type = T;
    return HighExpr::makeBinop(NdOp::INT_MULT, V, Factor);
  };
  return Form == 3 ? HighExpr::makeBinop(NdOp::INT_ADD, Left, Negative(Right))
                   : HighExpr::makeBinop(NdOp::INT_ADD, Negative(Left), Right);
}

TEST(HighSourceExpressions, WordZeroTestsBecomeEqualWidthComparisons) {
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (unsigned Form = 0; Form != 5; ++Form)
      for (bool Reverse : {false, true})
        for (const auto Op : {NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL}) {
          const auto T = NdType::makeInt(Bytes, true);
          auto L = parameter(0, T, Arch::X64);
          auto R = parameter(1, NdType::makeInt(Bytes, false), Arch::X64);
          auto Difference = wordDifference(L, R, Form);
          auto View = integerView(Difference, NdType::makeInt(Bytes, false));
          auto Zero = HighExpr::makeConst(0, Bytes);
          std::vector<HighStmt> Body{returning(nullptr)};
          Body[0].RetVal = HighExpr::makeBinop(Op, Reverse ? Zero : View,
                                               Reverse ? View : Zero);
          ASSERT_TRUE(simplifyAllExprs(Body));
          const auto &Result = Body[0].RetVal;
          ASSERT_EQ(Result->Kind, ExprKind::BinOp);
          ASSERT_EQ(Result->Op, Op);
          for (const auto &Operand : Result->Operands) {
            ASSERT_TRUE(Operand->Type);
            EXPECT_EQ(Operand->Type->Size, Bytes);
            if (Bytes < 4)
              EXPECT_FALSE(Operand->Type->IsSigned);
            EXPECT_TRUE(Operand->Kind == ExprKind::Var ||
                        Operand->Kind == ExprKind::Cast);
          }
          // Another use still observes the original arithmetic word.
          EXPECT_EQ(Difference->Kind, ExprKind::BinOp);
        }
}

TEST(HighSourceExpressions, WordZeroTestsPreserveWidthsAndEffects) {
  const auto T = NdType::makeInt(8, false);
  for (unsigned Case = 0; Case != 10; ++Case) {
    SCOPED_TRACE(Case);
    auto L = parameter(0, T, Arch::X64);
    auto R = parameter(1, T, Arch::X64);
    auto Difference = wordDifference(L, R, 2);
    ExprPtr Input = Difference;
    auto Zero = HighExpr::makeConst(0, 8);
    if (Case == 0)
      Input = integerView(Difference, NdType::makeInt(4, false));
    else if (Case == 1)
      Input = HighExpr::makeBitCast(Difference, NdType::makeFloat(8));
    else if (Case == 2)
      Difference->Operands.push_back(HighExpr::makeConst(7, 8));
    else if (Case == 3)
      Difference->Operands[0]->Operands[1]->ConstProvenance =
          ConstantAddressProvenance::CodeAddress;
    else if (Case == 4)
      Difference->Operands[0]->Operands[1]->ConstVal = 3;
    else if (Case == 5)
      Zero = HighExpr::makeConst(0, 4);
    else if (Case == 6) {
      auto Load = HighExpr::makeLoad(L, T);
      Load->MemoryOrdering = NdMemoryOrdering::Acquire;
      Difference->Operands[0]->Operands[0] = Load;
    } else if (Case == 7)
      Difference->IntrinsicOutputs = {temporary(9, T, Arch::X64)->Var};
    else if (Case == 8)
      Input = integerView(Difference, NdType::makeInt(16, false));
    else
      Difference->Operands[0] = HighExpr::makeUnary(NdOp::INT_NEGATE, L);
    std::vector<HighStmt> Body{
        returning(HighExpr::makeBinop(NdOp::INT_EQUAL, Input, Zero))};
    simplifyAllExprs(Body);
    ASSERT_EQ(Body[0].RetVal->Operands.size(), 2u);
    // No unproved comparison of the two original parameters.
    EXPECT_EQ(Body[0].RetVal->Operands[1], Zero);
  }
}

TEST(HighSourceExpressions, WordZeroTestsExecuteAtIntegerBoundaries) {
  std::string Source;
  std::vector<std::string> Checks;
  for (uint16_t Bytes : {1, 2, 4, 8})
    for (unsigned Form = 0; Form != 5; ++Form)
      for (bool Equal : {false, true}) {
        const auto T = NdType::makeInt(Bytes, true);
        const auto U = NdType::makeInt(Bytes, false);
        HighFunc F;
        const auto Suffix = std::to_string(Bytes) + "_" + std::to_string(Form) +
                            (Equal ? "_eq" : "_ne");
        F.Params = {{"arg0", T}, {"arg1", U}};
        F.ReturnType = NdType::makeInt(1, false);
        auto Difference = wordDifference(parameter(0, T, Arch::X64),
                                         parameter(1, U, Arch::X64), Form);
        F.Body = {returning(HighExpr::makeBinop(
            Equal ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
            HighExpr::makeConst(0, Bytes), integerView(Difference, U)))};
        CEmitterOptions Options;
        Options.TheArch = Arch::X64;
        for (bool Simplified : {false, true}) {
          F.Name = std::string(Simplified ? "after_" : "before_") + Suffix;
          if (Simplified)
            ASSERT_TRUE(simplifyAllExprs(F.Body));
          llvm::raw_string_ostream Out(Source);
          ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
        }
        const auto Width = std::to_string(Bytes * 8);
        const auto Arguments = "((int" + Width + "_t)x,(uint" + Width + "_t)y)";
        Checks.push_back("if (before_" + Suffix + Arguments + " != after_" +
                         Suffix + Arguments + " || after_" + Suffix +
                         Arguments + " != ((uint" + Width + "_t)x " +
                         (Equal ? "==" : "!=") + " (uint" + Width +
                         "_t)y)) return 1;\n");
      }
  Source += R"(
static int check(uint64_t x, uint64_t y) {
)";
  for (const auto &Check : Checks)
    Source += Check;
  Source += R"(
  return 0;
}
int main(void) {
  uint64_t values[]={0,1,127,128,255,256,32767,32768,65535,65536,
    UINT64_C(0x7fffffff),UINT64_C(0x80000000),UINT64_C(0xffffffff),
    UINT64_C(0x100000000),UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000),UINT64_MAX};
  for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i)
    for (unsigned j=0;j<sizeof(values)/sizeof(values[0]);++j)
      if (check(values[i],values[j])) return 2;
  for (uint64_t i=0;i<256;++i)
    for (uint64_t j=0;j<256;++j)
      if (check(i,j)) return 3;
  return 0;
}
)";
  compileAndRun(Source);
}

TEST(HighSourceExpressions, ZeroTestsKeepTruncationButDropOuterExtensions) {
  const auto T = NdType::makeInt(8, false);
  const auto Narrow = NdType::makeInt(4, true);
  for (auto Op : {NdOp::INT_ZEXT, NdOp::INT_SEXT}) {
    HighFunc F;
    F.Name = "low_word_zero";
    F.Params = {{"arg0", T}};
    F.ReturnType = NdType::makeInt(1, false);
    auto Low = integerView(parameter(0, T, Arch::X64), Narrow);
    auto Extended = HighExpr::makeUnary(Op, Low);
    Extended->Type = T;
    F.Body = {returning(HighExpr::makeBinop(NdOp::INT_EQUAL, Extended,
                                            HighExpr::makeConst(0, 8)))};
    ASSERT_TRUE(simplifyAllExprs(F.Body));
    EXPECT_EQ(F.Body[0].RetVal->Operands[0], Low);
    EXPECT_EQ(F.Body[0].RetVal->Operands[1]->Type->Size, 4u);
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    Source += R"(
int main(void) {
  return !low_word_zero(0) || !low_word_zero(UINT64_C(0x100000000)) ||
    low_word_zero(UINT64_C(0x80000000)) || low_word_zero(UINT64_MAX);
}
)";
    compileAndRun(Source);
  }
}

TEST(HighSourceExpressions, DifferenceComparisonsKeepEveryCallEvaluation) {
  std::string Source;
  HighFunc F;
  F.ReturnType = NdType::makeInt(1, false);
  auto Call = HighExpr::makeCall("observe_word", 0x2000, {});
  Call->Type = NdType::makeInt(8, false);
  F.Body = {returning(HighExpr::makeBinop(NdOp::INT_NOTEQUAL,
                                          wordDifference(Call, Call, 2),
                                          HighExpr::makeConst(0, 8)))};
  for (bool Simplified : {false, true}) {
    F.Name = Simplified ? "after_calls" : "before_calls";
    if (Simplified)
      ASSERT_TRUE(simplifyAllExprs(F.Body));
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  }
  Source += R"(
static unsigned calls;
int64_t observe_word(void) { ++calls; return 6; }
int main(void) {
  if (before_calls() || calls!=2) return 1;
  if (after_calls() || calls!=4) return 2;
  return 0;
}
)";
  compileAndRun(Source);
}

TEST(HighFrameStoreForwarding, CapturedCallResultReloadsAcrossABranch) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  for (bool Clobber : {false, true}) {
    auto F = roundTrip(A, T);
    auto Captured = temporary(20, T, A);
    auto Call = HighExpr::makeCall("produce", 0x2000, {});
    Call->Type = T;
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = parameter(0, T, A);
    if (Clobber)
      Branch.Body = {store(frameSlot(A), HighExpr::makeConst(77, 8))};
    F.Body = {assign(Captured, Call), store(frameSlot(A), Captured), Branch,
              returning(HighExpr::makeLoad(frameSlot(A), T))};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(hasLoad(F.Body.back().RetVal), Clobber);
    EXPECT_EQ(F.Body[0].Val, Call);
  }
}

TEST(HighFrameStoreForwarding, CapturedReloadDoesNotReplayAnExternalRead) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  HighFunc F;
  F.Name = "captured_load";
  F.FrameSize = 64;
  F.ReturnType = T;
  F.Params = {{"arg0", NdType::makePtr(T)}, {"arg1", T}};
  auto Captured = temporary(20, T, A);
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = parameter(1, T, A);
  Branch.Body = {
      store(parameter(0, NdType::makePtr(T), A), HighExpr::makeConst(77, 8))};
  F.Body = {assign(Captured,
                   HighExpr::makeLoad(parameter(0, NdType::makePtr(T), A), T)),
            store(frameSlot(A), Captured), Branch,
            returning(HighExpr::makeLoad(frameSlot(A), T))};
  // A possible aliasing external write invalidates the frame proof. Either
  // way the initial external read must remain a captured observation.
  forwardPrivateFrameLoads(F, A);
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = A;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  Source += R"(
int main(void) {
  uint64_t x=11;
  if (captured_load(&x,1)!=11 || x!=77) return 1;
  x=19;
  return captured_load(&x,0)!=19 || x!=19;
}
)";
  compileAndRun(Source);
}

TEST(HighFrameStoreForwarding, OverwritesMustCoverEverySourcePath) {
  for (const auto A : Architectures) {
    const auto T = NdType::makeInt(4, false);
    for (unsigned Variant = 0; Variant != 5; ++Variant) {
      SCOPED_TRACE(static_cast<int>(A));
      SCOPED_TRACE(Variant);
      auto F = roundTrip(A, T);
      HighStmt Branch;
      Branch.Kind = Variant == 1 ? StmtKind::If : StmtKind::IfElse;
      Branch.Cond = parameter(0, T, A);
      Branch.Body = {store(frameSlot(A), HighExpr::makeConst(11, 4))};
      if (Variant != 1)
        Branch.ElseBody = {store(frameSlot(A), HighExpr::makeConst(12, 4))};
      if (Variant == 2) {
        HighStmt Call;
        Call.Kind = StmtKind::Call;
        Call.CallExpr = HighExpr::makeCall("observe", 0x2000, {frameSlot(A)});
        Branch.Body.insert(Branch.Body.begin(), Call);
      }
      if (Variant == 3)
        Branch.Body[0].StoreVal = HighExpr::makeConst(11, 2);
      if (Variant == 4)
        Branch.Body[0].MemoryOrdering =
            NdMemoryOrdering::SequentiallyConsistent;
      F.Body.insert(F.Body.begin() + 1, Branch);
      ASSERT_TRUE(buildHighSourceFlowGraph(F).Diagnostics.Complete);
      forwardPrivateFrameLoads(F, A);
      EXPECT_EQ(F.Body[0].Kind == StmtKind::Block, Variant == 0);
    }
  }
}

TEST(HighFrameStoreForwarding, DeadUnknownSpillsDisappearOnlyBeforeOverwrite) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  auto F = roundTrip(A, T);
  F.Name = "overwritten_spill";
  auto Unknown = temporary(22, T, A);
  Unknown->Var.Kind = MedVar::Reg;
  Unknown->Var.RegOff = x86reg::RDI;
  F.Body[0].StoreVal = Unknown;
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = parameter(0, T, A);
  Branch.Body = {store(frameSlot(A), HighExpr::makeConst(11, 8))};
  Branch.ElseBody = {store(frameSlot(A), HighExpr::makeConst(12, 8))};
  F.Body.insert(F.Body.begin() + 1, Branch);
  ASSERT_TRUE(buildHighSourceFlowGraph(F).Diagnostics.Complete);
  forwardPrivateFrameLoads(F, A);
  ASSERT_EQ(F.Body[0].Kind, StmtKind::Block);
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = A;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
  EXPECT_EQ(Source.find("unknown register"), std::string::npos) << Source;
  Source += R"(
int main(void) {
  return overwritten_spill(0)!=12 || overwritten_spill(1)!=11;
}
)";
  compileAndRun(Source);
}

TEST(HighFrameStoreForwarding, RemovedWritebacksCannotKillTheirProducer) {
  for (const auto A : Architectures) {
    const auto T = NdType::makeInt(4, false);
    auto F = roundTrip(A, T);
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Cond = parameter(0, T, A);
    Branch.Body = {store(frameSlot(A), parameter(0, T, A))};
    Branch.ElseBody = Branch.Body;
    auto Observe = HighExpr::makeCall("observe", 0x2000, {frameSlot(A)});
    Observe->Type = T;
    F.Body = {store(frameSlot(A), parameter(0, T, A)), Branch,
              returning(Observe)};
    forwardPrivateFrameLoads(F, A);
    EXPECT_EQ(F.Body[0].Kind, StmtKind::Store);
    EXPECT_EQ(F.Body[1].Body[0].Kind, StmtKind::Block);
    EXPECT_EQ(F.Body[1].ElseBody[0].Kind, StmtKind::Block);
  }
}

TEST(HighFrameStoreForwarding, PointerBitViewsKeepCapturedParameterValues) {
  const auto A = Arch::X64;
  const auto Integer = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(Integer);
  for (bool Reassigned : {false, true}) {
    auto F = roundTrip(A, Integer);
    F.Name = "captured_parameter";
    F.FrameSize = 0;
    F.Params = {{"arg0", Pointer}, {"arg1", Pointer}};
    auto Alias = temporary(20, Integer, A);
    auto View = std::make_shared<HighExpr>();
    View->Kind = ExprKind::Cast;
    View->Type = View->CastTo = Integer;
    View->Operands = {parameter(0, Pointer, A)};
    F.Body = {assign(Alias, View)};
    if (Reassigned)
      F.Body.push_back(
          assign(parameter(0, Pointer, A), parameter(1, Pointer, A)));
    F.Body.push_back(store(Alias, HighExpr::makeConst(42, 8)));
    F.Body.push_back(returning(HighExpr::makeConst(0, 8)));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = A;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    Source += R"(
int main(void) {
  uint64_t first=11,second=23;
  captured_parameter(&first,&second);
  return first!=42 || second!=23;
})";
    compileAndRun(Source);
  }
}
TEST(HighSourceExpressions, PointerBitViewsKeepTheCapturedLocal) {
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  const auto P = NdType::makePtr(T);
  for (bool Reassigned : {false, true}) {
    HighFunc F;
    F.Name = "pointer_view";
    F.ReturnType = T;
    F.Params = {{"arg0", P}, {"arg1", P}};
    auto Captured = temporary(20, P, A);
    auto View = integerView(parameter(0, P, A), T);
    View = integerView(View, NdType::makeInt(8, true));
    F.Body = {assign(Captured, View)};
    if (Reassigned)
      F.Body.push_back(assign(parameter(0, P, A), parameter(1, P, A)));
    F.Body.push_back(store(Captured, HighExpr::makeConst(42, 8)));
    F.Body.push_back(returning(HighExpr::makeConst(0, 8)));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = A;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    if (Reassigned)
      EXPECT_NE(Source.find(" = arg0;"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("(uintptr_t)((int64_t)"), std::string::npos)
        << Source;
    Source += R"(
int main(void) {
  uint64_t first=11,second=23;
  pointer_view(&first,&second);
  return first!=42 || second!=23;
}
)";
    compileAndRun(Source);
  }
}
TEST(HighSourceExpressions,
     PointerBitViewsKeepNarrowingAndExplicitTypeChanges) {
  const auto T = NdType::makeInt(8, false);
  const auto P = NdType::makePtr(T);
  for (unsigned Case = 0; Case != 3; ++Case) {
    HighFunc F;
    F.Name = "pointer_view_boundary";
    F.ReturnType = T;
    F.Params = {{"arg0", P}};
    const auto Intermediate = Case == 0   ? NdType::makeInt(4, false)
                              : Case == 1 ? NdType::makeFloat(8)
                                          : NdType::makePtr(NdType::makeInt(1));
    auto View = integerView(parameter(0, P, Arch::X64), Intermediate);
    auto Captured = temporary(20, P, Arch::X64);
    F.Body = {assign(Captured, integerView(View, T)),
              returning(integerView(Captured, T))};
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options));
    EXPECT_EQ(Source.find(" = arg0;"), std::string::npos) << Source;
  }
}
TEST(HighSourceExpressions, CallbackDeclaratorsBorrowIncompleteRecords) {
  const auto Record = NdType::makeNamedRecord("Context", 24);
  const auto Pointer = NdType::makePtr(Record);
  const auto Callback =
      NdType::makePtr(NdType::makeFunc(NdType::makeVoid(), {Pointer}));
  EXPECT_EQ(declarationToC(Callback, "visit"), "void (*visit)(Context*)");
  EXPECT_THROW((void)declarationToC(NdType::makePtr(NdType::makeFunc(
                                        NdType::makeVoid(), {Record})),
                                    "visit"),
               std::invalid_argument);
  compileAndRun("typedef struct Context Context;\n" +
                declarationToC(Callback, "visit") +
                ";\nint main(void) { return visit!=0; }\n");
}
TEST(HighSourceExpressions, TypedIndirectCallUsesTheCapturedRuntimeTarget) {
  class CalleeTypes : public NullDebugContext {
  public:
    bool hasInfo() const override { return true; }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != 0x2000)
        return std::nullopt;
      FunctionSym F;
      F.Name = "signature_only";
      F.Addr = Address;
      F.ReturnType = NdType::makeInt(4, true);
      F.Params = {{"value", NdType::makeInt(8, false)}};
      return F;
    }
  } Debug;
  const auto A = Arch::X64;
  const auto T = NdType::makeInt(8, false);
  const auto Result = NdType::makeInt(4, true);
  HighFunc F;
  F.Name = "captured_callback";
  F.ReturnType = Result;
  F.Params = {{"arg0", T}, {"arg1", T}};
  auto Captured = temporary(20, T, A);
  auto Call = HighExpr::makeCall("indirect", 0x2000, {parameter(1, T, A)});
  Call->Type = Result;
  Call->IsIndirectCall = true;
  Call->IndirectTarget = integerView(Captured, NdType::makeInt(8, true));
  F.Body = {assign(Captured, parameter(0, T, A)),
            assign(parameter(0, T, A), HighExpr::makeConst(0, 8)),
            returning(Call)};
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = A;
  Options.Format = BinaryFormat::COFF;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options, &Debug));
  EXPECT_NE(Source.find("int32_t (*t20)(uint64_t)"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("t20(arg1)"), std::string::npos) << Source;
  Source += R"(
static int32_t actual(uint64_t x) { return (int32_t)x+7; }
int32_t signature_only(uint64_t x) { return -99; }
int main(void) {
  return captured_callback((uint64_t)(uintptr_t)&actual,5)!=12;
}
)";
  compileAndRun(Source);
}
} // namespace
