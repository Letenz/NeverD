//===- HighCSupervisorCallTests.cpp - Unmodeled SVC results
//----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/raw_ostream.h"

#include <stdexcept>

using namespace neverd;

namespace {
HighFunc supervisorCall(Arch Architecture, Intrinsic Operation,
                        bool UseResult) {
  const unsigned Bytes = Architecture == Arch::AArch64 ? 8 : 4;
  const auto Integer = NdType::makeInt(Bytes, false);
  MedVar Result;
  Result.Kind = MedVar::Reg;
  Result.Id = 0;
  Result.SSAVer = 1;
  Result.Size = Bytes;
  Result.TheArch = Architecture;
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeVar(Result, Integer);
  Assign.Val = HighExpr::makeCall("", 0, {HighExpr::makeConst(0, 4)});
  Assign.Val->IntrinsicId = Operation;
  Assign.Val->Type = Integer;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = UseResult ? HighExpr::makeBinop(NdOp::INT_ADD, Assign.Dst,
                                                  HighExpr::makeConst(1, Bytes))
                            : HighExpr::makeConst(1, Bytes);
  HighFunc Function;
  Function.Name = "supervisor_call";
  Function.ReturnType = Integer;
  Function.Body = {Assign, Return};
  // An explicit result contract prevents void-return inference from hiding
  // the unsupported live result that this test must exercise.
  SourceFunctionTypeHint Signature;
  Signature.ReturnType = Integer;
  Function.SourceTypeHint = Signature;
  return Function;
}

std::string emit(const HighFunc &Function, Arch Architecture) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.TheArch = Architecture;
  Options.Format = BinaryFormat::Raw;
  EXPECT_TRUE(HighCEmitter().emit({Function}, OS, Options));
  return Text;
}
} // namespace

TEST(HighCSupervisorCalls, LiveUnmodeledResultsFailWithoutAbortingTheHost) {
  for (const auto &[Architecture, Operation] :
       {std::pair{Arch::AArch64, Intrinsic::Svc},
        std::pair{Arch::ARM, Intrinsic::ArmSvc}}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    EXPECT_THROW(
        emit(supervisorCall(Architecture, Operation, true), Architecture),
        std::invalid_argument);
    // A refused function must not prevent the caller from emitting another.
    const auto Text =
        emit(supervisorCall(Architecture, Operation, false), Architecture);
    EXPECT_NE(Text.find("svc"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("linux_syscall"), std::string::npos) << Text;
  }
}
