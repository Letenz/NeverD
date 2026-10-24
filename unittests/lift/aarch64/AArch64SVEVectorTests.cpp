//===- AArch64SVEVectorTests.cpp - Canonical Z storage tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cross-element consumers, whole COPY/PHI transport, and strict admission
/// through production LLVM and HighC paths. Object generation is not native
/// SVE execution; the latter requires a suitable target.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/HighC/AArch64SVEVectorState.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/med/AArch64SVEState.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

namespace {

#define NEVERD_SVE_VECTOR_FIXTURE(Type, Name, Value)                           \
  constexpr Type Name = Value;
#include "AArch64SVEVectorFixtures.def"

constexpr unsigned Widths[] = {
#define NEVERD_SVE_VECTOR_WIDTH(Bytes) Bytes,
#include "AArch64SVEVectorFixtures.def"
};

enum class Transport { Copy, Diamond, Loop };

MedVar temporary(unsigned Id, uint16_t Bytes = kVectorBytes) {
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.TheArch = Arch::AArch64;
  Value.Id = Id;
  Value.Size = Bytes;
  return Value;
}

MedVar constant(uint64_t Value, uint16_t Bytes = kScalarBytes) {
  return MedVar::makeConst(Value, Bytes);
}

MedOp operation(NdOp Id, MedVar Output, std::initializer_list<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Id;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

MedOp intrinsic(Intrinsic Id, MedVar Output,
                std::initializer_list<MedVar> Inputs) {
  auto Op =
      operation(NdOp::INTRINSIC, Output, {constant(static_cast<uint64_t>(Id))});
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

MedFunc vectorFunction(unsigned Producer, unsigned Consumer, Transport Mode,
                       bool Dup = false, bool Store = false,
                       Intrinsic StoreId = Intrinsic::A64_SveSt1,
                       unsigned StoreBytes = kByteElement) {
  MedFunc F;
  F.Name = llvm::formatv(kNameFormat, Producer, Consumer,
                         static_cast<unsigned>(Mode))
               .str();
  F.Entry = kEntry;
  F.ReturnType = NdType::makeInt(kScalarBytes, false);
  F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  const auto Predicate = temporary(kPredicateId, kPredicateBytes);
  const auto Vector = temporary(kVectorId);
  const auto Copy = temporary(kCopyId);
  const auto Result = temporary(kResultId, kScalarBytes);
  MedVar Condition;
  Condition.Kind = MedVar::Param;
  Condition.TheArch = Arch::AArch64;
  Condition.Size = kScalarBytes;
  F.Params.push_back(Condition);
  const unsigned Blocks = Mode == Transport::Copy      ? 1
                          : Mode == Transport::Diamond ? 4
                                                       : 3;
  F.Blocks.resize(Blocks);
  for (unsigned I = 0; I != Blocks; ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = kEntry + I * kBlockStride;
    F.Blocks[I].EndAddr = F.Blocks[I].StartAddr + kBlockStride;
  }
  auto &Entry = F.Blocks[0];
  Entry.Ops.push_back(intrinsic(
      Intrinsic::A64_SvePtrue, Predicate,
      {constant(kVL2, kPatternBytes), constant(Consumer, kElementBytes)}));
  Entry.Ops.push_back(
      Dup ? intrinsic(
                Intrinsic::A64_SveDup, Vector,
                {constant(kIndexStart), constant(Producer, kElementBytes)})
          : intrinsic(Intrinsic::A64_SveIndex, Vector,
                      {constant(kIndexStart), constant(kIndexStep),
                       constant(Producer, kElementBytes)}));
  MedVar Consumed = Copy;
  if (Mode == Transport::Copy) {
    Entry.Ops.push_back(operation(NdOp::COPY, Copy, {Vector}));
  } else if (Mode == Transport::Diamond) {
    Entry.Succs = {1, 2};
    Entry.Ops.push_back(operation(
        NdOp::COND_BR, {}, {constant(F.Blocks[1].StartAddr), Condition}));
    const auto Alternative = temporary(kAlternateId);
    for (unsigned I : {1u, 2u}) {
      auto &Arm = F.Blocks[I];
      Arm.Preds = {0};
      Arm.Succs = {3};
      if (I == 1)
        Arm.Ops.push_back(operation(NdOp::COPY, Copy, {Vector}));
      else
        Arm.Ops.push_back(
            intrinsic(Intrinsic::A64_SveIndex, Alternative,
                      {constant(kAlternateStart), constant(kIndexStep),
                       constant(Consumer, kElementBytes)}));
      Arm.Ops.push_back(
          operation(NdOp::BRANCH, {}, {constant(F.Blocks[3].StartAddr)}));
    }
    auto &Join = F.Blocks[3];
    Join.Preds = {1, 2};
    Consumed = temporary(kJoinId);
    Join.Phis.push_back({Consumed, {{1, Copy}, {2, Alternative}}, {}});
  } else {
    Entry.Succs = {1};
    Entry.Ops.push_back(
        operation(NdOp::BRANCH, {}, {constant(F.Blocks[1].StartAddr)}));
    auto &Loop = F.Blocks[1];
    Loop.Preds = {0, 1};
    Loop.Succs = {1, 2};
    const auto NextVector = temporary(kLoopVectorId);
    Loop.Phis.push_back({Copy, {{0, Vector}, {1, NextVector}}, {}});
    const auto Counter = temporary(kLoopId, kScalarBytes);
    const auto Next = temporary(kNextId, kScalarBytes);
    const auto Test = temporary(kLoopTestId, kElementBytes);
    Loop.Phis.push_back({Counter, {{0, Condition}, {1, Next}}, {}});
    Loop.Ops.push_back(intrinsic(
        Intrinsic::A64_SveIndex, NextVector,
        {Counter, constant(kIndexStep), constant(Consumer, kElementBytes)}));
    Loop.Ops.push_back(operation(NdOp::INT_SUB, Next, {Counter, constant(1)}));
    Loop.Ops.push_back(
        operation(NdOp::INT_NOTEQUAL, Test, {Next, constant(0)}));
    Loop.Ops.push_back(
        operation(NdOp::COND_BR, {}, {constant(Loop.StartAddr), Test}));
    F.Blocks[2].Preds = {1};
  }
  auto &Exit = F.Blocks.back();
  if (Store)
    Exit.Ops.push_back(intrinsic(StoreId, {},
                                 {Predicate, Consumed, Condition,
                                  constant(Consumer, kElementBytes),
                                  constant(StoreBytes, kElementBytes)}));
  Exit.Ops.push_back(
      intrinsic(Intrinsic::A64_SveLastb, Result,
                {Predicate, Consumed, constant(Consumer, kElementBytes)}));
  Exit.Ops.push_back(operation(NdOp::RETURN, {}, {Result}));
  return F;
}

std::string highSource(const MedFunc &F) {
  const auto High = MedToHighConverter().convert(F, Arch::AArch64);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.EmitComments = false;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_TRUE(HighCEmitter().emit({High}, Out, Options));
  return Source;
}

void compileSource(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName(kCompiler);
  if (!Found)
    GTEST_SKIP() << kMissingCompiler;
  const std::string Compiler = *Found;
#endif
  llvm::SmallString<128> SourcePath, ObjectPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kSourceExtension,
                                                  SourcePath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kObjectExtension,
                                                  ObjectPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kErrorExtension,
                                                  ErrorPath));
  std::error_code Error;
  {
    llvm::raw_fd_ostream Out(SourcePath, Error);
    ASSERT_FALSE(Error);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const auto *Optimization : {kO0, kO2}) {
    const llvm::StringRef Args[] = {Compiler,       kTargetOption,
                                    kCTarget,       kMarch,
                                    kFreeStanding,  kNoSystemHeaders,
                                    kIncludeOption, NEVERD_SVE_TEST_HEADERS,
                                    kStandard,      Optimization,
                                    kCompileOption, SourcePath,
                                    kOutputOption,  ObjectPath};
    std::string ExecutionError;
    const int Status =
        llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt, Redirects,
                                  kTimeoutSeconds, 0, &ExecutionError);
    const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    // Preserve the source, object and stderr on failure for diagnosis.
    ASSERT_EQ(Status, 0) << SourcePath.str().str() << ObjectPath.str().str()
                         << ErrorPath.str().str() << ExecutionError
                         << (Errors ? (*Errors)->getBuffer().str()
                                    : std::string());
    uint64_t ObjectBytes = 0;
    ASSERT_FALSE(llvm::sys::fs::file_size(ObjectPath, ObjectBytes));
    EXPECT_GT(ObjectBytes, 0u);
  }
  EXPECT_FALSE(llvm::sys::fs::remove(SourcePath));
  EXPECT_FALSE(llvm::sys::fs::remove(ObjectPath));
  EXPECT_FALSE(llvm::sys::fs::remove(ErrorPath));
}

TEST(AArch64SVEVector, IndependentRawWordsPreserveTypedBoundaries) {
  struct Case {
    uint32_t Word;
    Intrinsic Id;
    unsigned Element;
  };
  const Case Cases[] = {
#define NEVERD_SVE_VECTOR_RAW(Word, Id, Element) {Word, Intrinsic::Id, Element},
#include "AArch64SVEVectorFixtures.def"
  };
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::AArch64));
  Decode.setStrict(true);
  for (const auto &Case : Cases)
    for (bool Fast : {false, true}) {
      std::array<uint8_t, kInstructionBytes> Bytes;
      writeLE<uint32_t>(Bytes.data(), Case.Word);
      DecodedInsn Instruction;
      ASSERT_EQ(Fast ? Decode.decodeOneForLift(Bytes.data(), Bytes.size(),
                                               kEntry, Instruction)
                     : Decode.decodeOne(Bytes.data(), Bytes.size(), kEntry,
                                        Instruction),
                Bytes.size());
      std::vector<LowOp> Ops;
      Decode.liftToLow(Instruction, Ops);
      ASSERT_EQ(Ops.size(), 1u);
      ASSERT_TRUE(lowAArch64VectorShapeIsValid(Ops.front()));
      EXPECT_EQ(Ops.front().Inputs[0].Offset, static_cast<uint64_t>(Case.Id));
      EXPECT_EQ(Ops.front().Inputs[Ops.front().NumInputs - 1].Offset,
                Case.Element);
    }
}

TEST(AArch64SVEVector, CrossElementCopyUsesOneCarrierInLLVMAndHighC) {
  for (unsigned Producer : Widths)
    for (unsigned Consumer : Widths) {
      SCOPED_TRACE(testing::Message() << Producer << '/' << Consumer);
      const auto F = vectorFunction(Producer, Consumer, Transport::Copy);
      ASSERT_TRUE(verifyMedFunc(F, kFunction));
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({F}, Context, kFunction, Arch::AArch64);
      ASSERT_TRUE(Module);
      ASSERT_FALSE(llvm::verifyModule(*Module));
      bool SawIndex = false, SawLast = false;
      for (const auto &Block : *Module->getFunction(F.Name))
        for (const auto &Inst : Block)
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst))
            if (const auto *Callee = Call->getCalledFunction()) {
              if (Callee->getIntrinsicID() ==
                  llvm::Intrinsic::aarch64_sve_index) {
                const auto *Ty =
                    llvm::cast<llvm::ScalableVectorType>(Call->getType());
                EXPECT_EQ(Ty->getElementCount().getKnownMinValue(),
                          kMinimumBytes / Producer);
                SawIndex = true;
              }
              if (Callee->getIntrinsicID() ==
                  llvm::Intrinsic::aarch64_sve_lastb) {
                const auto *Ty = llvm::cast<llvm::ScalableVectorType>(
                    Call->getArgOperand(1)->getType());
                EXPECT_EQ(Ty->getElementCount().getKnownMinValue(),
                          kMinimumBytes / Consumer);
                SawLast = true;
              }
            }
      EXPECT_TRUE(SawIndex);
      EXPECT_TRUE(SawLast);
      const auto Source = highSource(F);
      if (Producer == kByteElement && Consumer == kWideElement)
        EXPECT_NE(Source.find(kFromByteC), std::string::npos);
      compileSource(Source);
    }
}

TEST(AArch64SVEVector, WideIndexByteConsumerCompilesAcrossPhiAndLoop) {
  for (const auto Mode :
       {Transport::Copy, Transport::Diamond, Transport::Loop}) {
    const auto F = vectorFunction(kWideElement, kByteElement, Mode);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({F}, Context, kFunction, Arch::AArch64);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    std::string IR;
    llvm::raw_string_ostream Out(IR);
    Module->print(Out, nullptr);
    EXPECT_NE(IR.find(kWideIndexIR), std::string::npos);
    EXPECT_NE(IR.find(kByteLastIR), std::string::npos);
    const auto Native =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::ELF, kTriple);
    EXPECT_TRUE(Native.Success);
    EXPECT_FALSE(Native.ObjectData.empty());
    const auto Source = highSource(F);
    EXPECT_NE(Source.find(kCanonicalC), std::string::npos);
    EXPECT_NE(Source.find(kToByteC), std::string::npos);
    EXPECT_NE(Source.find(kByteLastC), std::string::npos);
    compileSource(Source);
  }
}

TEST(AArch64SVEVector, DupAndEveryStoreWidthUseCanonicalInput) {
  for (unsigned Producer : Widths)
    for (unsigned Consumer : Widths)
      for (unsigned Stored : Widths) {
        if (Stored > Consumer)
          continue;
        const auto F = vectorFunction(Producer, Consumer, Transport::Copy, true,
                                      true, Intrinsic::A64_SveSt1, Stored);
        compileSource(highSource(F));
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({F}, Context, kFunction, Arch::AArch64);
        ASSERT_TRUE(Module);
        EXPECT_FALSE(llvm::verifyModule(*Module));
      }
  for (unsigned Consumer : Widths) {
    const auto F = vectorFunction(kWideElement, Consumer, Transport::Copy, true,
                                  true, Intrinsic::A64_SveStnt1, Consumer);
    compileSource(highSource(F));
  }
}

TEST(AArch64SVEVector, MalformedVectorMetadataFailsBeforeEmission) {
  const auto Valid =
      vectorFunction(kWideElement, kByteElement, Transport::Copy);
  std::vector<MedFunc> BadFunctions;
  for (uint64_t Width : {uint64_t(kBadElement), kTruncatedElement}) {
    auto Bad = Valid;
    Bad.Blocks[0].Ops[1].Inputs[3] = constant(Width, kElementBytes);
    BadFunctions.push_back(std::move(Bad));
  }
  auto Dynamic = Valid;
  Dynamic.Blocks[0].Ops[1].Inputs[3] = temporary(kResultId, kElementBytes);
  BadFunctions.push_back(std::move(Dynamic));
  auto WrongWidth = Valid;
  WrongWidth.Blocks[0].Ops[1].Inputs[3].Size = kScalarBytes;
  BadFunctions.push_back(std::move(WrongWidth));
  auto Extra = Valid;
  Extra.Blocks[0].Ops[1].addInput(constant(0));
  BadFunctions.push_back(std::move(Extra));
  auto ConstantOutput = Valid;
  ConstantOutput.Blocks[0].Ops[1].Output = constant(0, kVectorBytes);
  BadFunctions.push_back(std::move(ConstantOutput));
  auto Ordered = Valid;
  Ordered.Blocks[0].Ops[1].MemoryOrdering = NdMemoryOrdering::Acquire;
  BadFunctions.push_back(std::move(Ordered));
  auto Auxiliary = Valid;
  Auxiliary.Blocks[0].Ops[1].IntrinsicOutputs.push_back(temporary(kCopyId));
  BadFunctions.push_back(std::move(Auxiliary));
  for (const auto &Bad : BadFunctions) {
    EXPECT_FALSE(aarch64VectorMedShapeIsValid(Bad.Blocks[0].Ops[1]));
    EXPECT_FALSE(verifyMedFunc(Bad, kFunction));
    llvm::LLVMContext Context;
    const std::vector<char> Mask{false};
    EXPECT_THROW(MedLLVMEmitter().emit({Bad}, Context, kFunction, Arch::AArch64,
                                       {}, nullptr, BinaryFormat::ELF, false,
                                       &Mask),
                 std::runtime_error);
  }
  llvm::LLVMContext Context;
  EXPECT_THROW(MedLLVMEmitter().emit({Valid}, Context, kFunction, Arch::X64),
               std::runtime_error);
  auto NarrowNonTemporal =
      vectorFunction(kWideElement, kWideElement, Transport::Copy, false, true,
                     Intrinsic::A64_SveStnt1, kByteElement);
  EXPECT_FALSE(
      aarch64VectorMedShapeIsValid(NarrowNonTemporal.Blocks[0].Ops[3]));
  EXPECT_THROW(MedLLVMEmitter().emit({NarrowNonTemporal}, Context, kFunction,
                                     Arch::AArch64),
               std::runtime_error);
}

ExprPtr highIntrinsic(Intrinsic Id, unsigned Bytes,
                      std::vector<ExprPtr> Operands) {
  auto E = HighExpr::makeCall({}, 0, std::move(Operands));
  E->IntrinsicId = Id;
  E->Type = NdType::makeInt(Bytes, false);
  return E;
}

HighStmt assignment(MedVar Dest, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = HighExpr::makeVar(Dest);
  S.Val = std::move(Value);
  return S;
}

HighFunc localVectorFunction() {
  HighFunc F;
  F.Name = kFunction;
  F.ReturnType = NdType::makeInt(kScalarBytes, false);
  const auto Vector = temporary(kVectorId);
  const auto Copy = temporary(kCopyId);
  const auto Join = temporary(kJoinId);
  F.Body.push_back(assignment(
      Vector,
      highIntrinsic(Intrinsic::A64_SveIndex, kVectorBytes,
                    {HighExpr::makeConst(kIndexStart, kScalarBytes),
                     HighExpr::makeConst(kIndexStep, kScalarBytes),
                     HighExpr::makeConst(kWideElement, kElementBytes)})));
  F.Body.push_back(assignment(Copy, HighExpr::makeVar(Vector)));
  F.Body.push_back(assignment(Join, HighExpr::makeVar(Copy)));
  // A lowered loop backedge can form a whole-register transport cycle.
  F.Body.push_back(assignment(Copy, HighExpr::makeVar(Join)));
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = highIntrinsic(
      Intrinsic::A64_SveLastb, kScalarBytes,
      {HighExpr::makeVar(temporary(kPredicateId, kPredicateBytes)),
       HighExpr::makeVar(Join),
       HighExpr::makeConst(kByteElement, kElementBytes)});
  F.Body.push_back(std::move(Return));
  return F;
}

std::set<std::string> localVectorNames(const HighFunc &F) {
  return aarch64SVEVectorLocalNames(F, Arch::AArch64, [](const MedVar &V) {
    return llvm::formatv(kValueNameFormat, V.Id).str();
  });
}

TEST(AArch64SVEVector, LocalClosureRequiresProvenDefinitionsAndWholeTransport) {
  const auto Valid = localVectorFunction();
  const auto Names = localVectorNames(Valid);
  for (unsigned Id : {kVectorId, kCopyId, kJoinId})
    EXPECT_TRUE(Names.count(llvm::formatv(kValueNameFormat, Id).str()));

  auto Unknown = localVectorFunction();
  Unknown.Body.erase(Unknown.Body.begin());
  EXPECT_THROW(localVectorNames(Unknown), std::invalid_argument);
  auto Parameter = localVectorFunction();
  auto Input = temporary(kJoinId);
  Input.Kind = MedVar::Param;
  Parameter.Body.back().RetVal->Operands[1] = HighExpr::makeVar(Input);
  EXPECT_THROW(localVectorNames(Parameter), std::invalid_argument);
  auto Floating = localVectorFunction();
  Floating.Body.front().Val->Operands.front()->Type =
      NdType::makeFloat(kScalarBytes);
  EXPECT_THROW(localVectorNames(Floating), std::invalid_argument);
  auto Integer = localVectorFunction();
  Integer.Body.front().Val = HighExpr::makeConst(0, kVectorBytes);
  EXPECT_THROW(localVectorNames(Integer), std::invalid_argument);
  auto Partial = localVectorFunction();
  Partial.Body[1].Val =
      HighExpr::makeBitCast(HighExpr::makeVar(temporary(kVectorId)),
                            NdType::makeInt(kVectorBytes, false));
  EXPECT_THROW(localVectorNames(Partial), std::invalid_argument);
  auto Returned = localVectorFunction();
  Returned.Body.back().RetVal = HighExpr::makeVar(temporary(kJoinId));
  EXPECT_THROW(localVectorNames(Returned), std::invalid_argument);
  auto AddressEscape = localVectorFunction();
  HighStmt IndirectWrite;
  IndirectWrite.Kind = StmtKind::Assign;
  IndirectWrite.Dst =
      HighExpr::makeLoad(HighExpr::makeVar(temporary(kVectorId)),
                         NdType::makeInt(kScalarBytes, false));
  IndirectWrite.Val = HighExpr::makeConst(0, kScalarBytes);
  AddressEscape.Body.push_back(std::move(IndirectWrite));
  EXPECT_THROW(localVectorNames(AddressEscape), std::invalid_argument);
  auto Mixed = localVectorFunction();
  Mixed.Body.insert(
      Mixed.Body.end() - 1,
      assignment(temporary(kJoinId), HighExpr::makeUndef(kVectorBytes)));
  EXPECT_THROW(localVectorNames(Mixed), std::invalid_argument);

  // A fixed-width value of the same byte size, with no SVE provenance,
  // must not acquire a scalable declaration merely from its size.
  auto Ordinary = localVectorFunction();
  Ordinary.Body.clear();
  Ordinary.Body.push_back(
      assignment(temporary(kVectorId), HighExpr::makeConst(0, kVectorBytes)));
  EXPECT_TRUE(localVectorNames(Ordinary).empty());
}

TEST(AArch64SVEVector, HighAndLowShapesUseTheSharedElementContract) {
  for (const auto Width : {uint64_t(kBadElement), kTruncatedElement}) {
    auto F = localVectorFunction();
    auto &Call = *F.Body.front().Val;
    Call.Operands.back() = HighExpr::makeConst(Width, kElementBytes);
    EXPECT_FALSE(aarch64VectorHighShapeIsValid(Call));
    EXPECT_THROW(localVectorNames(F), std::invalid_argument);
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = NdVar::tmp(kVectorId, kVectorBytes);
    Op.addInput(NdVar::cst(static_cast<uint64_t>(Intrinsic::A64_SveIndex),
                           kLowIdBytes));
    Op.addInput(NdVar::cst(kIndexStart, kScalarBytes));
    Op.addInput(NdVar::cst(kIndexStep, kScalarBytes));
    Op.addInput(NdVar::cst(Width, kElementBytes));
    EXPECT_FALSE(lowAArch64VectorShapeIsValid(Op));
  }
  auto F = localVectorFunction();
  auto &Call = *F.Body.front().Val;
  Call.Operands.back()->Type = NdType::makeInt(kScalarBytes, false);
  EXPECT_FALSE(aarch64VectorHighShapeIsValid(Call));
  EXPECT_THROW(localVectorNames(F), std::invalid_argument);
}

TEST(AArch64SVEVector, NarrowSignedScalarKeepsItsOriginalBitsWhenWidened) {
  for (bool Dup : {false, true}) {
    auto F = vectorFunction(kWideElement, kWideElement, Transport::Copy, Dup);
    auto &Producer = F.Blocks[0].Ops[1];
    Producer.Inputs[1] = constant(kNarrowScalarBits, kSignedScalarBytes);
    if (!Dup)
      Producer.Inputs[2] = constant(kNarrowScalarBits, kSignedScalarBytes);
    ASSERT_TRUE(aarch64VectorMedShapeIsValid(Producer));
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({F}, Context, kFunction, Arch::AArch64);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    bool Found = false;
    for (const auto &Block : *Module->getFunction(F.Name))
      for (const auto &Inst : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst))
          if (const auto *Callee = Call->getCalledFunction())
            if (Callee->getIntrinsicID() ==
                (Dup ? llvm::Intrinsic::aarch64_sve_dup_x
                     : llvm::Intrinsic::aarch64_sve_index)) {
              for (unsigned I = 0; I != Call->arg_size(); ++I) {
                const auto *Bits =
                    llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(I));
                ASSERT_TRUE(Bits);
                EXPECT_EQ(Bits->getZExtValue(), kNarrowScalarBits);
              }
              Found = true;
            }
    EXPECT_TRUE(Found);
    bool UsesIntrinsics = false;
    const std::vector<std::string> Operands =
        Dup ? std::vector<std::string>{kSignedNarrowC, kWideElementC}
            : std::vector<std::string>{kSignedNarrowC, kSignedNarrowC,
                                       kWideElementC};
    const std::vector<uint16_t> OperandBytes =
        Dup ? std::vector<uint16_t>{kSignedScalarBytes, kElementBytes}
            : std::vector<uint16_t>{kSignedScalarBytes, kSignedScalarBytes,
                                    kElementBytes};
    EXPECT_EQ(renderARMIntrinsicCall(
                  Dup ? Intrinsic::A64_SveDup : Intrinsic::A64_SveIndex,
                  Operands, kVectorBytes, UsesIntrinsics, OperandBytes),
              Dup ? kNarrowDupC : kNarrowIndexC);
    EXPECT_TRUE(UsesIntrinsics);
    const auto Source = highSource(F);
    EXPECT_NE(Source.find(kNarrowCastC), std::string::npos);
    compileSource(Source);
    EXPECT_THROW(renderIntrinsicCall(
                     Dup ? Intrinsic::A64_SveDup : Intrinsic::A64_SveIndex,
                     Arch::AArch64, Operands, kVectorBytes, UsesIntrinsics),
                 std::invalid_argument);
  }
}

TEST(AArch64SVEVector, LLVMCRejectsScalableCarrierExplicitly) {
  llvm::LLVMContext Context;
  auto *Vector = llvm::ScalableVectorType::get(llvm::Type::getInt8Ty(Context),
                                               kMinimumBytes);
  EXPECT_THROW(typeToCLLVM(Vector), std::invalid_argument);
}

} // namespace

namespace {

void expectStateRejected(const MedFunc &Function) {
  EXPECT_NE(validateAArch64SVEState(Function, Arch::AArch64), nullptr);
  EXPECT_THROW(MedToHighConverter().convert(Function, Arch::AArch64),
               std::runtime_error);
  for (bool EmitBody : {false, true}) {
    llvm::LLVMContext Context;
    const std::vector<char> Mask{static_cast<char>(EmitBody)};
    EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, kFunction,
                                       Arch::AArch64, {}, nullptr,
                                       BinaryFormat::ELF, true, &Mask),
                 std::runtime_error);
  }
}

} // namespace

TEST(AArch64SVEVector, SharedStateAdmissionRejectsUnknownAndLateDefinitions) {
  for (unsigned Missing : {kPredicateOperation, kVectorOperation}) {
    auto Function = vectorFunction(kWideElement, kByteElement, Transport::Copy);
    Function.Blocks.front().Ops.erase(Function.Blocks.front().Ops.begin() +
                                      Missing);
    expectStateRejected(Function);
  }
  auto Self = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  auto &Producer = Self.Blocks.front().Ops[kVectorOperation];
  Producer = operation(NdOp::COPY, Producer.Output, {Producer.Output});
  expectStateRejected(Self);
  auto Late = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  std::swap(Late.Blocks.front().Ops[kVectorOperation],
            Late.Blocks.front().Ops[kCopyOperation]);
  expectStateRejected(Late);
  auto Duplicate = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  const auto Extra = Duplicate.Blocks.front().Ops[kVectorOperation];
  Duplicate.Blocks.front().Ops.insert(Duplicate.Blocks.front().Ops.begin(),
                                      Extra);
  expectStateRejected(Duplicate);
  auto Dead = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  Dead.Blocks.front().Ops[kVectorOperation].Dead = true;
  expectStateRejected(Dead);
  auto Width = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  Width.Blocks.front().Ops[kCopyOperation].Inputs.front().Size = kScalarBytes;
  expectStateRejected(Width);
}

TEST(AArch64SVEVector, SharedStateAdmissionRequiresEveryJoinPath) {
  auto Missing = vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  Missing.Blocks[kAlternateBlock].Ops.erase(
      Missing.Blocks[kAlternateBlock].Ops.begin());
  expectStateRejected(Missing);
  auto WrongEdge =
      vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  auto &Phi = WrongEdge.Blocks.back().Phis.front();
  Phi.Args.front().second = Phi.Args.back().second;
  expectStateRejected(WrongEdge);
  auto MissingEdge =
      vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  MissingEdge.Blocks.back().Phis.front().Args.pop_back();
  expectStateRejected(MissingEdge);
  auto DuplicateEdge =
      vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  auto &Args = DuplicateEdge.Blocks.back().Phis.front().Args;
  Args.back().first = Args.front().first;
  expectStateRejected(DuplicateEdge);
  auto Exceptional =
      vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  Exceptional.Blocks.back().Phis.front().ExceptionalEntry =
      temporary(kVectorId);
  expectStateRejected(Exceptional);
  auto UnknownLoop =
      vectorFunction(kWideElement, kByteElement, Transport::Loop);
  auto &LoopPhi = UnknownLoop.Blocks[kLoopBlock].Phis.front();
  LoopPhi.Args.front().second = LoopPhi.Output;
  expectStateRejected(UnknownLoop);
}

TEST(AArch64SVEVector, SharedStateAdmissionRejectsCallsAndScalableABI) {
  for (const auto Opcode : {NdOp::CALL, NdOp::INDIR_CALL, NdOp::INDIR_BR}) {
    auto Function = vectorFunction(kWideElement, kByteElement, Transport::Copy);
    auto Call = operation(Opcode, {}, {constant(kEntry)});
    Call.PreservesCallerSaved = true;
    Function.Blocks.front().Ops.insert(
        Function.Blocks.front().Ops.begin() + kCopyOperation, Call);
    expectStateRejected(Function);
  }
  auto System = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  System.Blocks.front().Ops.insert(
      System.Blocks.front().Ops.begin(),
      intrinsic(Intrinsic::Svc, {}, {constant(0)}));
  expectStateRejected(System);
  auto Clobber = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  MedCallClobber Record;
  Record.CallSiteId = kClobberSite;
  Record.Value = temporary(kVectorId);
  Clobber.CallClobbers.push_back(Record);
  expectStateRejected(Clobber);
  auto Parameter = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  MedVar Input = temporary(kParameterId);
  Input.Kind = MedVar::Param;
  Parameter.Params.push_back(Input);
  Parameter.Blocks.front().Ops[kCopyOperation].Inputs.front() = Input;
  expectStateRejected(Parameter);
  auto Returned = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  Returned.Blocks.back().Ops.back().Inputs.front() = temporary(kCopyId);
  expectStateRejected(Returned);
  auto ResultType = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  ResultType.ReturnType = NdType::makeInt(kVectorBytes, false);
  expectStateRejected(ResultType);
}

TEST(AArch64SVEVector, SharedStateAdmissionRejectsUntypedTransport) {
  auto Loaded = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  Loaded.Blocks.front().Ops[kVectorOperation] =
      operation(NdOp::LOAD, temporary(kVectorId), {Loaded.Params.front()});
  expectStateRejected(Loaded);
  auto Stored = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  Stored.Blocks.front().Ops.insert(
      Stored.Blocks.front().Ops.end() - 1,
      operation(NdOp::STORE, {}, {Stored.Params.front(), temporary(kCopyId)}));
  expectStateRejected(Stored);
  auto Partial = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  Partial.Blocks.front().Ops.insert(
      Partial.Blocks.front().Ops.end() - 1,
      operation(NdOp::SUBBYTES, temporary(kPartialId, kScalarBytes),
                {temporary(kCopyId), constant(0)}));
  expectStateRejected(Partial);
  auto Unspecified =
      vectorFunction(kWideElement, kByteElement, Transport::Copy);
  Unspecified.Blocks.front().Ops[kCopyOperation].Inputs.front() =
      MedVar::makeUnspecified(kVectorBytes, Arch::AArch64);
  expectStateRejected(Unspecified);
  auto Mutable = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  Mutable.SkippedSSA = true;
  expectStateRejected(Mutable);
}

TEST(AArch64SVEVector, MixedPhysicalAliasesWaitForExplicitBridge) {
  auto Function = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  MedVar Vector = temporary(kVectorId);
  Vector.Kind = MedVar::Reg;
  Vector.RegOff = kPhysicalZ0;
  Function.Blocks.front().Ops[kVectorOperation].Output = Vector;
  Function.Blocks.front().Ops[kCopyOperation].Inputs.front() = Vector;
  MedVar Neon = temporary(kNeonId, kNeonBytes);
  Neon.Kind = MedVar::Reg;
  Neon.RegOff = kPhysicalV0;
  Function.Blocks.front().Ops.insert(
      Function.Blocks.front().Ops.begin(),
      operation(NdOp::COPY, Neon, {constant(kIndexStart, kNeonBytes)}));
  expectStateRejected(Function);

  MedFunc Ordinary;
  Ordinary.Name = kFunction;
  Ordinary.ReturnType = NdType::makeInt(kScalarBytes, false);
  Ordinary.Blocks.emplace_back();
  Ordinary.Blocks.front().Id = 0;
  Ordinary.Blocks.front().Ops.push_back(
      operation(NdOp::COPY, Neon, {constant(kIndexStart, kNeonBytes)}));
  Ordinary.Blocks.front().Ops.push_back(
      operation(NdOp::RETURN, {}, {constant(kIndexStart)}));
  EXPECT_EQ(validateAArch64SVEState(Ordinary, Arch::AArch64), nullptr);
  llvm::LLVMContext Context;
  const auto Module =
      MedLLVMEmitter().emit({Ordinary}, Context, kFunction, Arch::AArch64);
  ASSERT_TRUE(Module);
  EXPECT_FALSE(llvm::verifyModule(*Module));
}

TEST(AArch64SVEVector, SharedStateAdmissionAllowsClosedCopiesAndLoopPhis) {
  for (const auto Mode :
       {Transport::Copy, Transport::Diamond, Transport::Loop}) {
    const auto Function = vectorFunction(kWideElement, kByteElement, Mode);
    EXPECT_EQ(validateAArch64SVEState(Function, Arch::AArch64), nullptr);
    EXPECT_NE(validateAArch64SVEState(Function, Arch::X64), nullptr);
  }
}

TEST(AArch64SVEVector, SharedStateAdmissionRejectsUnexecutedDefinitions) {
  for (const auto Opcode : {NdOp::BRANCH, NdOp::COND_BR, NdOp::RETURN}) {
    auto Function = vectorFunction(kWideElement, kByteElement, Transport::Copy);
    Function.Blocks.resize(kTwoBlocks);
    auto &Entry = Function.Blocks.front();
    auto &Exit = Function.Blocks.back();
    Exit.Id = kExitBlock;
    Exit.StartAddr = kEntry + kBlockStride;
    Exit.EndAddr = Exit.StartAddr + kBlockStride;
    Exit.Preds = {0};
    Entry.Succs = {kExitBlock};
    Exit.Ops.assign(Entry.Ops.end() - kConsumerAndReturn, Entry.Ops.end());
    Entry.Ops.erase(Entry.Ops.end() - kConsumerAndReturn, Entry.Ops.end());
    auto Early = operation(Opcode, {}, {constant(Exit.StartAddr)});
    if (Opcode == NdOp::COND_BR)
      Early.addInput(Function.Params.front());
    Entry.Ops.insert(Entry.Ops.begin(), Early);
    expectStateRejected(Function);
  }
  auto WrongSuccessors =
      vectorFunction(kWideElement, kByteElement, Transport::Copy);
  WrongSuccessors.Blocks.front().Succs.push_back(0);
  expectStateRejected(WrongSuccessors);
  auto Fallthrough =
      vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  Fallthrough.Blocks.front().Ops.pop_back();
  expectStateRejected(Fallthrough);
}

TEST(AArch64SVEVector, SharedStateAdmissionBoundsProofResources) {
  auto Function = vectorFunction(kWideElement, kByteElement, Transport::Copy);
  Function.Blocks.resize(kExcessiveBlocks);
  EXPECT_STREQ(validateAArch64SVEState(Function, Arch::AArch64),
               kStateLimitText);
  expectStateRejected(Function);
}

TEST(AArch64SVEVector, SharedStateAdmissionRequiresActualBranchTarget) {
  auto Wrong = vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  Wrong.Blocks.front().Ops.back().Inputs.front() = constant(kUnlistedTarget);
  expectStateRejected(Wrong);
  auto Dynamic = vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  Dynamic.Blocks.front().Ops.back().Inputs.front() = Dynamic.Params.front();
  expectStateRejected(Dynamic);
  auto Branch = vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  Branch.Blocks[kLoopBlock].Ops.back().Inputs.front() =
      constant(kUnlistedTarget);
  expectStateRejected(Branch);
  auto Zero = vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  Zero.Blocks[kLoopBlock].StartAddr = 0;
  Zero.Blocks.front().Ops.back().Inputs.front() = constant(0);
  expectStateRejected(Zero);
  auto Duplicate =
      vectorFunction(kWideElement, kByteElement, Transport::Diamond);
  Duplicate.Blocks[kAlternateBlock].StartAddr =
      Duplicate.Blocks[kLoopBlock].StartAddr;
  expectStateRejected(Duplicate);
}
