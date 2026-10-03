//===- BytecodeDecoderTests.cpp - Original bytecode language checks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/analysis/BytecodeDecoder.h"
#include "neverd/analysis/BytecodeSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"

using namespace neverd;
using namespace neverd::analysis;

namespace {

BytecodeOperand constant(uint64_t Value, uint16_t Size = 8) {
  BytecodeOperand O;
  O.Size = Size;
  O.Value.Addend = Value;
  return O;
}

BytecodeOperand reg(uint16_t ByteOffset, uint16_t Size = 8) {
  auto O = constant(0, Size);
  O.Space = VnodeSpace::REG;
  O.Value.Offset = ByteOffset;
  O.Value.Bytes = 1;
  O.Value.Scale = 8;
  return O;
}

BytecodeOperand fixedReg(uint64_t Offset, uint16_t Size = 8) {
  auto O = constant(Offset, Size);
  O.Space = VnodeSpace::REG;
  return O;
}

BytecodeProfile toyProfile() {
  BytecodeProfile P;
  P.RegisterBytes = 32;
  auto Add = [&](uint8_t Opcode, uint16_t Size, BytecodeOperation Op) {
    P.Encodings.push_back({Size, {{0, 255, Opcode}}, {std::move(Op)}});
  };
  auto Immediate = constant(0, 2);
  Immediate.Value.Offset = 2;
  Immediate.Value.Bytes = 2;
  Add(0xb4, 4, {NdOp::COPY, reg(1, 2), {Immediate}});
  Add(0x91, 3, {NdOp::INT_ADD, reg(1), {reg(1), reg(2)}});
  Add(0x7c, 2, {NdOp::INT_SUB, reg(1), {reg(1), constant(1)}});
  Add(0xa7, 2, {NdOp::INT_NOTEQUAL, fixedReg(24, 1), {reg(1), constant(0)}});
  auto Target = constant(0);
  Target.Value.Bytes = 4;
  Target.Value.Offset = 1;
  Target.Value.PCRelative = true;
  Target.Value.ValueBits = 32;
  Add(0x80, 5, {NdOp::COND_BR, {}, {Target, fixedReg(24, 1)}});
  Add(0xd2, 3, {NdOp::STORE, {}, {reg(1), reg(2)}});
  Add(0xe7, 1, {NdOp::RETURN, {}, {}});
  return P;
}

const std::vector<uint8_t> ToyProgram{0xb4, 0,    0x34, 0x12, 0x91, 0,    1,
                                      0x7c, 1,    0xa7, 1,    0x80, 0xf9, 0xff,
                                      0xff, 0xff, 0xd2, 2,    0,    0xe7};

llvm::Expected<std::unique_ptr<BytecodeDecoder>>
sourceDecoder(BytecodeProfile P, bool External) {
  if (!External)
    return BytecodeDecoder::create(std::move(P));
  auto Encodings = std::move(P.Encodings);
  P.Encodings.clear();
  return BytecodeDecoder::createExternal(
      std::move(P),
      [Encodings = std::move(Encodings)](llvm::ArrayRef<uint8_t> Bytes, va_t)
          -> llvm::Expected<BytecodeEncoding> {
        for (auto E : Encodings)
          if (Bytes.front() == E.Match.front().Value) {
            E.Match.clear();
            return E;
          }
        return llvm::createStringError(llvm::errc::invalid_argument,
                                       "unrecognized synthetic opcode");
      });
}

TEST(BytecodeDecoder, ExternalLayoutsAndRepliesRejectAmbiguousContracts) {
  auto Layout = readBytecodeLayout(
      R"({"version":1,"register_bytes":8,"byte_order":"big"})");
  ASSERT_TRUE(bool(Layout));
  auto Recipe = readBytecodeEncoding(R"({"size":3,"operations":[
    {"op":"COPY","output":{"space":"reg","size":2,"value":{}},
     "inputs":[{"space":"const","size":2,"value":{"offset":1,"bytes":2}}]}]})");
  ASSERT_TRUE(bool(Recipe));
  auto D = BytecodeDecoder::createExternal(
      *Layout,
      [Recipe = *Recipe](llvm::ArrayRef<uint8_t>, va_t) { return Recipe; });
  ASSERT_TRUE(bool(D));
  auto I = (*D)->decode(std::vector<uint8_t>{0x99, 0x12, 0x34}, 16);
  ASSERT_TRUE(bool(I));
  EXPECT_EQ(I->Encoding, UINT32_MAX);
  EXPECT_EQ(I->Operations[0].Inputs[0], NdVar::scalar(0x1234, 2));
  EXPECT_EQ(I->Operations[0].Output, NdVar::reg(0, 2));
  EXPECT_EQ(I->Operations[0].Addr, 16u);
  for (llvm::StringRef Bad :
       {R"({"size":1,"match":[],"operations":[]})", R"({"error":""})",
        R"({"error":1})", R"({"error":"failed","size":1})",
        R"({"size":1,"operations":[{"op":"UNKNOWN","inputs":[]}]})"}) {
    auto Result = readBytecodeEncoding(Bad);
    EXPECT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
  }
  auto Failed = readBytecodeEncoding(R"({"error":"missing decoding context"})");
  ASSERT_FALSE(bool(Failed));
  EXPECT_NE(llvm::toString(Failed.takeError()).find("missing decoding context"),
            std::string::npos);
  auto BadLayout = readBytecodeLayout(
      R"({"version":1,"register_bytes":8,"byte_order":"little","encodings":[]})");
  EXPECT_FALSE(bool(BadLayout));
  llvm::consumeError(BadLayout.takeError());
  auto NoCallback = BytecodeDecoder::createExternal(*Layout, {});
  EXPECT_FALSE(bool(NoCallback));
  llvm::consumeError(NoCallback.takeError());
  Layout->Encodings.push_back(*Recipe);
  auto Mixed = BytecodeDecoder::createExternal(
      *Layout,
      [Recipe = *Recipe](llvm::ArrayRef<uint8_t>, va_t) { return Recipe; });
  EXPECT_FALSE(bool(Mixed));
  llvm::consumeError(Mixed.takeError());
}

TEST(BytecodeDecoder, ExternalRecipesShareOperandAndTemporaryValidation) {
  BytecodeProfile Layout;
  Layout.RegisterBytes = 8;
  auto Temp = constant(0);
  Temp.Space = VnodeSpace::TEMP;
  for (unsigned Case = 0; Case != 12; ++Case) {
    SCOPED_TRACE(Case);
    BytecodeEncoding E{1, {}, {{NdOp::COPY, fixedReg(0), {constant(7)}}}};
    switch (Case) {
    case 0:
      E.Size = 0;
      break;
    case 1:
      E.Size = 2;
      break; // A reply cannot read past the input window.
    case 2:
      E.Match = {{0, 255, 0x42}};
      break;
    case 3:
      E.Operations[0].Output = fixedReg(1);
      break;
    case 4:
      E.Operations[0].Inputs[0] = Temp;
      break;
    case 5:
      E.Operations = {{NdOp::COPY, Temp, {constant(1)}},
                      {NdOp::COPY, fixedReg(0, 4), {Temp}}};
      E.Operations.back().Inputs[0].Size = 4;
      break;
    case 6:
      E.Operations = {{NdOp::COPY, Temp, {constant(1)}},
                      {NdOp::COPY, Temp, {constant(2, 4)}},
                      {NdOp::COPY, fixedReg(0), {Temp}}};
      E.Operations[1].Output.Size = 4;
      break;
    case 7:
      E.Operations[0].Inputs[0].Value.Bytes = 8;
      break;
    case 8:
      E.Operations[0].Inputs[0].Size = 4;
      break;
    case 9:
      E.Operations[0].Opcode = NdOp::_COUNT;
      break;
    case 10:
      E.Operations.insert(E.Operations.begin(), {NdOp::RETURN, {}, {}});
      break;
    case 11:
      E.Operations.clear();
      break;
    }
    auto D = BytecodeDecoder::createExternal(
        Layout, [E](llvm::ArrayRef<uint8_t>, va_t) { return E; });
    ASSERT_TRUE(bool(D));
    auto I = (*D)->decode(std::vector<uint8_t>{0x42}, 0);
    ASSERT_FALSE(bool(I));
    llvm::consumeError(I.takeError());
  }
}

TEST(BytecodeDecoder, ExternalDecodingRetainsCFGBudgetsAndBoundaries) {
  auto D = sourceDecoder(toyProfile(), true);
  ASSERT_TRUE(bool(D));
  auto Good = (*D)->function(ToyProgram, 0, 0, ToyProgram.size(), "external");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->DecodedBytes, ToyProgram.size());
  EXPECT_EQ(Good->Function.Blocks.size(), 3u);
  for (unsigned Case = 0; Case != 5; ++Case) {
    auto Code = ToyProgram;
    BytecodeDecodeLimits Limits;
    if (Case == 0)
      Code[12] = 0xfa; // Overlap an operand.
    if (Case == 1)
      Code[12] = 0x10; // Branch outside the declared function.
    if (Case == 2)
      Code[0] = 0;
    if (Case == 3)
      Limits.MaxInstructions = 2;
    if (Case == 4)
      Limits.MaxOperations = 2;
    auto F = (*D)->function(Code, 0, 0, Code.size(), "bad", Limits);
    EXPECT_FALSE(bool(F)) << Case;
    llvm::consumeError(F.takeError());
  }
}

TEST(BytecodeDecoder, ExternalWindowsAreBoundedEvenForLargeFunctions) {
  BytecodeProfile Layout;
  Layout.RegisterBytes = 8;
  std::vector<size_t> Windows;
  auto D = BytecodeDecoder::createExternal(
      Layout, [&](llvm::ArrayRef<uint8_t> Bytes, va_t PC) {
        Windows.push_back(Bytes.size());
        return BytecodeEncoding{uint16_t(PC ? 1 : 4096),
                                {},
                                {{PC ? NdOp::RETURN : NdOp::NOP, {}, {}}}};
      });
  ASSERT_TRUE(bool(D));
  std::vector<uint8_t> Code(4097, 0);
  auto F = (*D)->function(Code, 0, 0, Code.size(), "window");
  ASSERT_TRUE(bool(F)) << llvm::toString(F.takeError());
  EXPECT_EQ(Windows, (std::vector<size_t>{4096, 1}));
  EXPECT_EQ(F->DecodedBytes, 4097u);
}

TEST(BytecodeDecoder, ExactWidthsFieldsAndControlFlow) {
  auto D = BytecodeDecoder::create(toyProfile());
  ASSERT_TRUE(bool(D)) << llvm::toString(D.takeError());
  auto I = (*D)->decode(ToyProgram, 0);
  ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
  EXPECT_EQ(I->Operations.front().Output, NdVar::reg(0, 2));
  EXPECT_EQ(I->Operations.front().Inputs[0], NdVar::scalar(0x1234, 2));
  auto F = (*D)->function(ToyProgram, 0, 0, ToyProgram.size(), "toy");
  ASSERT_TRUE(bool(F)) << llvm::toString(F.takeError());
  EXPECT_EQ(F->DecodedBytes, ToyProgram.size());
  EXPECT_EQ(F->Function.DecodedInstructionCount, 7u);
  ASSERT_EQ(F->Function.Blocks.size(), 3u);
  EXPECT_EQ(F->Function.Blocks[0].StartAddr, 0u);
  EXPECT_EQ(F->Function.Blocks[0].EndAddr, 4u);
  EXPECT_EQ(F->Function.Blocks[1].StartAddr, 4u);
  EXPECT_EQ(F->Function.Blocks[1].EndAddr, 16u);
  EXPECT_EQ(F->Function.Blocks[1].Succs, (std::vector<int>{1, 2}));
  EXPECT_TRUE(F->DirectCalls.empty());
  for (const auto &B : F->Function.Blocks)
    EXPECT_TRUE(B.InstructionBoundaries.empty());
}

TEST(BytecodeDecoder, RejectsTruncationUnknownAmbiguityAndInvalidRegisters) {
  auto D = BytecodeDecoder::create(toyProfile());
  ASSERT_TRUE(bool(D));
  for (unsigned N = 0; N != 4; ++N) {
    auto I = (*D)->decode(llvm::ArrayRef(ToyProgram).take_front(N), 0);
    EXPECT_FALSE(bool(I));
    llvm::consumeError(I.takeError());
  }
  for (const std::vector<uint8_t> B :
       {std::vector<uint8_t>{0x42}, {0xb4, 4, 0, 0}, {0x91, 0xff, 0}}) {
    auto I = (*D)->decode(B, 0);
    EXPECT_FALSE(bool(I));
    llvm::consumeError(I.takeError());
  }
  auto P = toyProfile();
  P.Encodings.push_back(P.Encodings.front());
  D = BytecodeDecoder::create(std::move(P));
  ASSERT_TRUE(bool(D));
  auto I = (*D)->decode(ToyProgram, 0);
  ASSERT_FALSE(bool(I));
  EXPECT_NE(llvm::toString(I.takeError()).find("ambiguous"), std::string::npos);
}

TEST(BytecodeDecoder, RefusesOverlappingControlAndExhaustedBudgets) {
  auto D = BytecodeDecoder::create(toyProfile());
  ASSERT_TRUE(bool(D));
  auto Bad = ToyProgram;
  Bad[12] = 0xfa; // Branch into the register operand of the add instruction.
  auto F = (*D)->function(Bad, 0, 0, Bad.size(), "bad");
  EXPECT_FALSE(bool(F));
  llvm::consumeError(F.takeError());
  for (auto Limits :
       {BytecodeDecodeLimits{2, 100}, BytecodeDecodeLimits{100, 2}}) {
    F = (*D)->function(ToyProgram, 0, 0, ToyProgram.size(), "budget", Limits);
    EXPECT_FALSE(bool(F));
    EXPECT_NE(llvm::toString(F.takeError()).find("budget"), std::string::npos);
  }
}

TEST(BytecodeDecoder, CallsDoNotMergeFunctionBoundaries) {
  auto P = toyProfile();
  P.Encodings.push_back(
      {1, {{0, 255, 0x33}}, {{NdOp::CALL, {}, {constant(0x2000)}}}});
  auto D = BytecodeDecoder::create(std::move(P));
  ASSERT_TRUE(bool(D));
  const std::vector<uint8_t> B{0x33, 0xe7};
  auto F = (*D)->function(B, 0x1000, 0x1000, 0x1002, "caller");
  ASSERT_TRUE(bool(F)) << llvm::toString(F.takeError());
  EXPECT_EQ(F->DirectCalls, std::set<va_t>{0x2000});
  EXPECT_EQ(F->DecodedBytes, 2u);
  ASSERT_EQ(F->Function.Blocks.size(), 2u);
  EXPECT_EQ(F->Function.Blocks.front().EndAddr, 0x1001u);
  auto Source = lowerBytecodeState(F->Function, 32);
  EXPECT_FALSE(bool(Source));
  llvm::consumeError(Source.takeError());
}

TEST(BytecodeDecoder, ByteOrderLookupAndTemporaryDefinedness) {
  auto P = toyProfile();
  P.ByteOrder = llvm::endianness::big;
  P.Encodings.front().Operations.front().Output.Value.Lookup = {2, 0, 1};
  auto D = BytecodeDecoder::create(P);
  ASSERT_TRUE(bool(D));
  auto I = (*D)->decode(ToyProgram, 0);
  ASSERT_TRUE(bool(I));
  EXPECT_EQ(I->Operations.front().Output.Offset, 16u);
  EXPECT_EQ(I->Operations.front().Inputs[0].Offset, 0x3412u);
  P.Encodings.front().Operations.front().Inputs[0].Space = VnodeSpace::TEMP;
  P.Encodings.front().Operations.front().Inputs[0].Value = {};
  D = BytecodeDecoder::create(std::move(P));
  ASSERT_TRUE(bool(D));
  I = (*D)->decode(ToyProgram, 0);
  EXPECT_FALSE(bool(I));
  EXPECT_NE(llvm::toString(I.takeError()).find("before definition"),
            std::string::npos);
}

TEST(BytecodeDecoder, ExternalProfileIsStrictAndHasNoBuiltInDialect) {
  const char *Text = R"({"version":1,"register_bytes":32,"byte_order":"little",
    "encodings":[{"size":1,"match":[{"offset":0,"value":"0xf1"}],
      "operations":[{"op":"RETURN","inputs":[]}]}]})";
  auto P = readBytecodeProfile(Text);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  auto D = BytecodeDecoder::create(std::move(*P));
  ASSERT_TRUE(bool(D));
  auto I = (*D)->decode(std::vector<uint8_t>{0xf1}, 0);
  ASSERT_TRUE(bool(I));
  for (std::string Bad : {"{}", "{\"version\":1,\"guess_unknown\":true}",
                          "{\"version\":2}", "[]", "{\"version\":-1}"}) {
    auto R = readBytecodeProfile(Bad);
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
}

TEST(BytecodeDecoder, TemporariesRequireExactDefinedValues) {
  auto Wide = constant(0);
  Wide.Space = VnodeSpace::TEMP;
  auto Narrow = Wide;
  Narrow.Size = 4;
  BytecodeProfile P;
  P.RegisterBytes = 8;
  P.Encodings = {{1,
                  {{0, 255, 0x61}},
                  {{NdOp::COPY, Wide, {constant(UINT64_MAX)}},
                   {NdOp::COPY, fixedReg(0, 4), {Narrow}}}}};
  auto D = BytecodeDecoder::create(P);
  ASSERT_TRUE(bool(D));
  auto I = (*D)->decode(std::vector<uint8_t>{0x61}, 0);
  ASSERT_FALSE(bool(I));
  llvm::consumeError(I.takeError());
  // A partial write invalidates the older full-width value. It cannot be
  // reconstructed by the SSA layer from a differently sized temporary.
  P.Encodings[0].Operations[1] = {NdOp::COPY, Narrow, {constant(1, 4)}};
  P.Encodings[0].Operations.push_back({NdOp::COPY, fixedReg(0), {Wide}});
  D = BytecodeDecoder::create(P);
  ASSERT_TRUE(bool(D));
  I = (*D)->decode(std::vector<uint8_t>{0x61}, 0);
  ASSERT_FALSE(bool(I));
  llvm::consumeError(I.takeError());
}

TEST(BytecodeDecoder, FullUnsignedConstantsAndMalformedFields) {
  auto P = readBytecodeProfile(R"({"version":1,"register_bytes":8,
    "byte_order":"little","encodings":[{"size":1,
    "match":[{"offset":0,"value":81}],"operations":[{"op":"COPY",
    "output":{"space":"reg","size":8,"value":{}},
    "inputs":[{"space":"const","size":8,
               "value":{"addend":18446744073709551615}}]}]}]})");
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  auto D = BytecodeDecoder::create(*P);
  ASSERT_TRUE(bool(D));
  auto I = (*D)->decode(std::vector<uint8_t>{81}, 0);
  ASSERT_TRUE(bool(I));
  EXPECT_EQ(I->Operations[0].Inputs[0].Offset, UINT64_MAX);
  for (unsigned Case = 0; Case != 5; ++Case) {
    auto Bad = *P;
    auto &Value = Bad.Encodings[0].Operations[0].Inputs[0].Value;
    if (Case == 0)
      Value.Bytes = 3;
    if (Case == 1)
      Value.ValueBits = 0;
    if (Case == 2)
      Value.Shift = 64;
    if (Case == 3)
      Value.Offset = 2;
    if (Case == 4)
      Value.Bytes = 8;
    auto Rejected = BytecodeDecoder::create(std::move(Bad));
    EXPECT_FALSE(bool(Rejected));
    llvm::consumeError(Rejected.takeError());
  }
}

class BytecodeSourceTest : public NeverDLiftTest {};

class BytecodeDecoderSourceTest : public BytecodeSourceTest,
                                  public ::testing::WithParamInterface<bool> {};
INSTANTIATE_TEST_SUITE_P(ProfileAndCallback, BytecodeDecoderSourceTest,
                         ::testing::Bool());

TEST_F(BytecodeSourceTest,
       ForwardedStateViewsRespectGuestAliasesAndPartialWrites) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source runtime checks require clang";
  BytecodeProfile P;
  P.RegisterBytes = 80;
  P.Encodings.push_back(
      {1,
       {{0, 255, 0x62}},
       {{NdOp::COPY, fixedReg(0), {fixedReg(8)}},
        {NdOp::INT_ADD, fixedReg(0), {fixedReg(0), constant(7)}}}});
  P.Encodings.push_back(
      {1,
       {{0, 255, 0x63}},
       {{NdOp::COPY, fixedReg(16), {fixedReg(0)}},
        {NdOp::COPY, fixedReg(3, 2), {constant(0xa1b2, 2)}},
        {NdOp::COPY, fixedReg(24), {fixedReg(0)}},
        {NdOp::COPY, fixedReg(0), {constant(0x1122334488776655)}},
        {NdOp::STORE, {}, {fixedReg(32), constant(0xfeedbeef)}},
        {NdOp::COPY, fixedReg(40), {fixedReg(8)}},
        {NdOp::COPY, fixedReg(48), {fixedReg(16)}},
        {NdOp::LOAD, fixedReg(56), {fixedReg(64)}},
        {NdOp::RETURN, {}, {}}}});
  auto D = BytecodeDecoder::create(std::move(P));
  ASSERT_TRUE(bool(D));
  const uint8_t Program[] = {0x62, 0x63};
  auto F = (*D)->function(Program, 0, 0, 2, "state_aliases");
  ASSERT_TRUE(bool(F));
  ASSERT_EQ(F->Function.Blocks.size(), 1u);
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    auto Bound = lowerBytecodeState(F->Function, 80, A);
    ASSERT_TRUE(bool(Bound));
    std::map<va_t, SourceFunctionTypeHint> Hints{{0, Bound->SourceABI}};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceEntryTypeHints(&Hints);
    auto Med = Converter.convert(Bound->Function, A);
    Med.SourceTypeHint = Bound->SourceABI;
    inferMedTypes(Med, A);
    ASSERT_TRUE(verifyMedFunc(Med, "bytecode-state-aliases"));
    for (auto [LLVM, Pointers] : {std::pair{false, false},
                                  {true, false},
                                  {false, true},
                                  {true, true}}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      SCOPED_TRACE(Pointers);
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.Format = BinaryFormat::ELF;
      Options.PreserveLLVMFunctionTypes = true;
      Options.UseUnalignedPointers = Pointers;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      if (LLVM) {
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({Med}, Context, "bytecode-test", A);
        ASSERT_NE(Module, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
      } else {
        auto High = MedToHighConverter().convert(Med, A);
        ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      }
      OS.flush();
      if (Pointers) {
        EXPECT_NE(Source.find("neverd_unaligned_u64 *)(uintptr_t)"),
                  std::string::npos);
        EXPECT_EQ(Source.find("neverd_mem_load"), std::string::npos);
        EXPECT_EQ(Source.find("neverd_mem_store"), std::string::npos);
      }
      const auto Path = tmpFile("state-aliases.c");
      std::ofstream(Path) << Source << R"(
#include <string.h>
int main(void) {
  const int targets[] = {-1, 0, 8, 17, 40};
  for (unsigned trial = 0; trial < 128; ++trial) {
    for (unsigned j = 0; j < sizeof(targets)/sizeof(targets[0]); ++j) {
      /* Unaligned state and overlapping writes must preserve every byte. */
      uint8_t bytes[82], expected[82];
      uint8_t *s = bytes + 1, *e = expected + 1;
      for (unsigned k = 0; k < sizeof(bytes); ++k)
        bytes[k] = (uint8_t)(trial * 13 + k * 71);
      uint64_t outside = 0, value = (uint64_t)(uintptr_t)(
          targets[j] < 0 ? (void *)&outside : (void *)(s + targets[j]));
      memcpy(s + 32, &value, 8);
      value = (uint64_t)(uintptr_t)s;
      memcpy(s + 64, &value, 8);
      memcpy(expected, bytes, sizeof(bytes));
      memcpy(e, e + 8, 8);
      memcpy(&value, e, 8); value += 7; memcpy(e, &value, 8);
      memcpy(e + 16, e, 8);
      e[3] = 0xb2; e[4] = 0xa1;
      memcpy(e + 24, e, 8);
      value = UINT64_C(0x1122334488776655); memcpy(e, &value, 8);
      value = 0xfeedbeef;
      if (targets[j] >= 0) memcpy(e + targets[j], &value, 8);
      memcpy(e + 40, e + 8, 8);
      memcpy(e + 48, e + 16, 8);
      memcpy(e + 56, e, 8);
      if (state_aliases(s) || memcmp(bytes, expected, sizeof(bytes)) ||
          outside != (targets[j] < 0 ? UINT64_C(0xfeedbeef) : 0))
        return 1;
    }
  }
  return 0;
}
)";
      for (const char *Opt : {"-O0", "-O2"}) {
        const auto Run = tmpFile(std::string("state-aliases") +
                                 neverd::test::executableSuffix());
        auto Built = exec(NEVERD_TEST_CLANG,
                          {"-std=c11", "-Werror", Opt, "-fsanitize=undefined",
                           "-fsanitize-trap=undefined", Path.string(), "-o",
                           Run.string()});
        ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
        EXPECT_TRUE(exec(Run.string(), {}).ok()) << Source;
      }
    }
  }
}

TEST_F(BytecodeSourceTest, LLVMIntegerBoundaryLiteralsCompileWithoutWarnings) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source runtime checks require clang";
  llvm::LLVMContext Context;
  llvm::Module Module("integer-boundary", Context);
  llvm::IRBuilder<> B(Context);
  auto *Type = llvm::FunctionType::get(B.getInt64Ty(), {B.getInt64Ty()}, false);
  auto *F = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                   "minimum_equal", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
  auto *Minimum = B.getInt64(UINT64_C(1) << 63);
  B.CreateRet(
      B.CreateZExt(B.CreateICmpEQ(F->getArg(0), Minimum), B.getInt64Ty()));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  auto Path = tmpFile("integer-boundary.c");
  std::ofstream(Path) << Source << R"(
int main(void) {
  uint64_t minimum = UINT64_C(1) << 63;
  return minimum_equal(minimum) != 1 || minimum_equal(0) != 0 ||
         minimum_equal(minimum - 1) != 0 || minimum_equal(minimum + 1) != 0;
}
)";
  for (const char *Opt : {"-O0", "-O2"}) {
    auto Program = tmpFile(std::string("integer-boundary") +
                           neverd::test::executableSuffix());
    auto Built =
        exec(NEVERD_TEST_CLANG, {"-std=c11", "-Werror", Opt, Path.string(),
                                 "-o", Program.string()});
    ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
    ASSERT_TRUE(exec(Program.string(), {}).ok());
  }
}

TEST_F(BytecodeSourceTest, BothCRoutesPreserveSignedDivisionAndExtension) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source runtime checks require clang";
  auto P = toyProfile();
  auto T = constant(0);
  T.Space = VnodeSpace::TEMP;
  P.Encodings.push_back(
      {1,
       {{0, 255, 0x6d}},
       {{NdOp::INT_SEXT, T, {constant(0xfffd, 2)}},
        {NdOp::INT_SDIV, fixedReg(0), {fixedReg(0), T}},
        {NdOp::INT_SREM, fixedReg(8), {fixedReg(8), T}},
        {NdOp::INT_SDIV, fixedReg(16, 4), {fixedReg(16, 4), constant(3, 4)}},
        {NdOp::INT_SREM, fixedReg(24, 4), {fixedReg(24, 4), constant(7, 4)}}}});
  auto D = BytecodeDecoder::create(std::move(P));
  ASSERT_TRUE(bool(D));
  const std::vector<uint8_t> Program{0x6d, 0xe7};
  auto F = (*D)->function(Program, 0, 0, 2, "signed_execute");
  ASSERT_TRUE(bool(F));
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    auto Wrapped = lowerBytecodeState(F->Function, 32, A);
    ASSERT_TRUE(bool(Wrapped));
    std::map<va_t, SourceFunctionTypeHint> Hints{{0, Wrapped->SourceABI}};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceEntryTypeHints(&Hints);
    auto Med = Converter.convert(Wrapped->Function, A);
    Med.SourceTypeHint = Wrapped->SourceABI;
    inferMedTypes(Med, A);
    ASSERT_TRUE(verifyMedFunc(Med, "bytecode-signed"));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      SCOPED_TRACE(static_cast<unsigned>(A));
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.Format = BinaryFormat::ELF;
      if (LLVM) {
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({Med}, Context, "bytecode-test", A);
        ASSERT_NE(Module, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
      } else {
        auto High = MedToHighConverter().convert(Med, A);
        ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      }
      OS.flush();
      auto Path = tmpFile("signed.c");
      std::ofstream(Path) << Source << R"(
int main(void) {
  const uint64_t values[] = {0, 1, 7, UINT64_MAX, UINT64_MAX - 15,
    UINT64_C(1) << 63, (UINT64_C(1) << 63) - 1, 0x80000000, 0xffffffff};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    uint64_t n = values[i], state[4] = {n, n, n, n};
    uint64_t upper = n & UINT64_C(0xffffffff00000000);
    if (signed_execute((uint8_t *)state) != 0 ||
        state[0] != (uint64_t)((int64_t)n / -3) ||
        state[1] != (uint64_t)((int64_t)n % -3) ||
        state[2] != (upper | (uint32_t)((int32_t)n / 3)) ||
        state[3] != (upper | (uint32_t)((int32_t)n % 7)))
      return 1;
  }
  return 0;
}
)";
      for (const char *Opt : {"-O0", "-O2"}) {
        auto Output = tmpFile(std::string("signed-test") +
                              neverd::test::executableSuffix());
        auto Built = exec(NEVERD_TEST_CLANG,
                          {"-std=c11", "-Werror", Opt, "-fsanitize=undefined",
                           "-fsanitize-trap=undefined", Path.string(), "-o",
                           Output.string()});
        ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
        ASSERT_TRUE(exec(Output.string(), {}).ok()) << Source;
      }
    }
  }
}

TEST_F(BytecodeSourceTest, FloatingConversionsKeepArchitectureResultPolicies) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source runtime checks require clang";
  auto P = toyProfile();
  P.RegisterBytes = 96;
  BytecodeEncoding Convert;
  Convert.Size = 1;
  Convert.Match = {{0, 255, 0xc6}};
  for (unsigned Input = 0; Input != 2; ++Input)
    for (unsigned Kind = 0; Kind != 4; ++Kind)
      Convert.Operations.push_back(
          {Kind & 1 ? NdOp::FLOAT_FLOAT2UINT : NdOp::FLOAT_FLOAT2INT,
           fixedReg(16 + Input * 32 + Kind * 8, Kind < 2 ? 4 : 8),
           {fixedReg(Input * 8, Input ? 8 : 4)}});
  P.Encodings.push_back(std::move(Convert));
  auto D = BytecodeDecoder::create(std::move(P));
  ASSERT_TRUE(bool(D));
  const uint8_t Program[] = {0xc6, 0xe7};
  auto F = (*D)->function(Program, 0, 0, 2, "convert_values");
  ASSERT_TRUE(bool(F)) << llvm::toString(F.takeError());
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    auto Wrapped = lowerBytecodeState(F->Function, 96, A);
    ASSERT_TRUE(bool(Wrapped));
    std::map<va_t, SourceFunctionTypeHint> Hints{{0, Wrapped->SourceABI}};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceEntryTypeHints(&Hints);
    auto Med = Converter.convert(Wrapped->Function, A);
    Med.SourceTypeHint = Wrapped->SourceABI;
    inferMedTypes(Med, A);
    for (bool LLVM : {false, true})
      for (bool Pointers : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(Pointers);
        std::string Source;
        llvm::raw_string_ostream OS(Source);
        CEmitterOptions Options;
        Options.TheArch = A;
        Options.UseUnalignedPointers = Pointers;
        if (LLVM) {
          llvm::LLVMContext Context;
          auto Module =
              MedLLVMEmitter().emit({Med}, Context, "bytecode-test", A);
          ASSERT_NE(Module, nullptr);
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
        } else {
          auto High = MedToHighConverter().convert(Med, A);
          ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
        }
        OS.flush();
        const auto Path = tmpFile("float-conversions.c");
        std::ofstream(Path) << "#define X86_POLICY " << (A == Arch::X64) << "\n"
                            << Source << R"(
#include <string.h>
int main(void) {
  const uint64_t u32 = UINT32_MAX, u64 = UINT64_MAX;
  const uint64_t s32 = UINT64_C(1) << 31, s64 = UINT64_C(1) << 63;
  const struct { double input; uint64_t expected[4]; } cases[] = {
    {0, {0, 0, 0, 0}}, {-0.75, {0, 0, 0, 0}}, {3.75, {3, 3, 3, 3}},
    {-3.75, {u32-2, X86_POLICY ? u32 : 0, u64-2, X86_POLICY ? u64 : 0}},
    {0x1p31, {X86_POLICY ? s32 : s32-1, s32, s32, s32}},
    {0x1p32, {X86_POLICY ? s32 : s32-1, u32, UINT64_C(1)<<32, UINT64_C(1)<<32}},
    {0x1p63, {X86_POLICY ? s32 : s32-1, u32, X86_POLICY ? s64 : s64-1, s64}},
    {-0x1p63, {s32, X86_POLICY ? u32 : 0, s64, X86_POLICY ? u64 : 0}},
    {0x1p64, {X86_POLICY ? s32 : s32-1, u32, X86_POLICY ? s64 : s64-1, u64}},
    {__builtin_inf(), {X86_POLICY ? s32 : s32-1, u32, X86_POLICY ? s64 : s64-1, u64}},
    {-__builtin_inf(), {s32, X86_POLICY ? u32 : 0, s64, X86_POLICY ? u64 : 0}},
    {__builtin_nan(""), {X86_POLICY ? s32 : 0, X86_POLICY ? u32 : 0,
                        X86_POLICY ? s64 : 0, X86_POLICY ? u64 : 0}}
  };
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
    unsigned char storage[98];
    memset(storage, 0xa5, sizeof(storage));
    unsigned char *state = storage + 1;
    float single = (float)cases[i].input;
    memcpy(state, &single, 4);
    memcpy(state + 8, &cases[i].input, 8);
    if (convert_values(state)) return 1;
    for (unsigned input = 0; input < 2; ++input)
      for (unsigned kind = 0; kind < 4; ++kind) {
        uint64_t value = 0;
        unsigned offset = 16 + input * 32 + kind * 8;
        unsigned size = kind < 2 ? 4 : 8;
        memcpy(&value, state + offset, size);
        if (value != cases[i].expected[kind]) return 2;
        for (unsigned byte = size; byte < 8; ++byte)
          if (state[offset + byte] != 0xa5) return 3;
      }
    for (unsigned byte = 80; byte < 96; ++byte)
      if (state[byte] != 0xa5) return 4;
    if (storage[0] != 0xa5 || storage[97] != 0xa5) return 5;
  }
  return 0;
}
)";
        for (const char *Opt : {"-O0", "-O2"}) {
          const auto Output = tmpFile(std::string("fp-test") +
                                      neverd::test::executableSuffix());
          auto Built = exec(NEVERD_TEST_CLANG,
                            {"-std=c11", "-Werror", Opt, "-fsanitize=undefined",
                             "-fsanitize-trap=undefined", Path.string(), "-o",
                             Output.string()});
          ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
          ASSERT_TRUE(exec(Output.string(), {}).ok()) << Source;
        }
      }
  }
}

TEST_P(BytecodeDecoderSourceTest, BoundCallsRetainStateAndPropagateFailure) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source runtime checks require clang";
  auto P = toyProfile();
  P.RegisterBytes = 40;
  P.Encodings.push_back(
      {1, {{0, 255, 0x33}}, {{NdOp::CALL, {}, {constant(0x2000)}}}});
  P.Encodings.push_back(
      {1, {{0, 255, 0x34}}, {{NdOp::CALL, {}, {constant(0x3000)}}}});
  P.Encodings.push_back(
      {1,
       {{0, 255, 0x55}},
       {{NdOp::INT_ADD, fixedReg(0), {fixedReg(0), constant(1)}}}});
  auto D = sourceDecoder(std::move(P), GetParam());
  ASSERT_TRUE(bool(D));
  // A language-level call inside a countdown loop; the child has a second
  // call and an observable store after it. A failed callback must skip both
  // post-call increments and leave the countdown unchanged for that iteration.
  const std::vector<uint8_t> Caller{0x33, 0x55, 0x7c, 1,    0xa7, 1,
                                    0x80, 0xfa, 0xff, 0xff, 0xff, 0xe7};
  const std::vector<uint8_t> Child{0x55, 0x34, 0x55, 0xe7};
  auto F = (*D)->function(Caller, 0x1000, 0x1000, 0x100c, "toy_caller");
  auto G = (*D)->function(Child, 0x2000, 0x2000, 0x2004, "toy_child");
  ASSERT_TRUE(bool(F)) << llvm::toString(F.takeError());
  ASSERT_TRUE(bool(G)) << llvm::toString(G.takeError());
  std::map<va_t, std::string> Names{
      {0x1000, "toy_caller"}, {0x2000, "toy_child"}, {0x3000, "toy_callback"}};
  const std::vector<va_t> Bindings{0x1000, 0x2000, 0x3000};
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    std::vector<LowFunc> Low;
    std::map<va_t, SourceFunctionTypeHint> Hints;
    for (const auto *Input : {&F->Function, &G->Function}) {
      auto Wrapped = lowerBytecodeState(*Input, 40, A, 10000, Bindings);
      ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
      Hints.emplace(Input->Entry, Wrapped->SourceABI);
      Low.push_back(std::move(Wrapped->Function));
    }
    Hints.emplace(0x3000, Hints.begin()->second);
    std::vector<MedFunc> Functions;
    for (const auto &Input : Low) {
      LowToMedConverter Converter;
      Converter.setSourceCallHintsEnabled(true);
      Converter.setSourceEntryTypeHints(&Hints);
      Converter.setSourceCalleeTypeHints(&Hints);
      auto Med = Converter.convert(Input, A);
      Med.SourceTypeHint = Hints.at(Input.Entry);
      for (auto &Block : Med.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL) {
            ASSERT_NE(Op.SourceCallHint, nullptr);
            auto Hint =
                std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
            Hint->TargetName = Names.at(Hint->TargetAddress);
            Op.SourceCallHint = std::move(Hint);
          }
      inferMedTypes(Med, A);
      recoverCallAbi(Med, A, Names);
      ASSERT_TRUE(verifyMedFunc(Med, "bytecode-call"));
      ASSERT_EQ(Med.Params.size(), 1u);
      Functions.push_back(std::move(Med));
    }
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.Format = BinaryFormat::ELF;
      Options.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      if (LLVM) {
        llvm::LLVMContext Context;
        auto WithImports = Functions;
        MedFunc Import;
        Import.Entry = 0x3000;
        Import.Name = "toy_callback";
        Import.Params = Functions.front().Params;
        Import.TypedParams = Functions.front().TypedParams;
        Import.ReturnType = Hints.at(0x3000).ReturnType;
        WithImports.push_back(std::move(Import));
        const std::vector<char> Bodies{1, 1, 0};
        auto Module =
            MedLLVMEmitter().emit(WithImports, Context, "calls", A, {}, nullptr,
                                  BinaryFormat::ELF, false, &Bodies);
        ASSERT_NE(Module, nullptr);
        auto *External = Module->getFunction("toy_callback");
        ASSERT_NE(External, nullptr);
        ASSERT_TRUE(External->isDeclaration());
        ASSERT_EQ(External->arg_size(), 1u);
        ASSERT_TRUE(External->getArg(0)->getType()->isPointerTy());
        ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
      } else {
        MedToHighConverter Converter;
        Converter.setFuncNames(&Names);
        std::vector<HighFunc> High;
        for (const auto &Med : Functions)
          High.push_back(Converter.convert(Med, A));
        ASSERT_TRUE(HighCEmitter().emit(High, OS, Options));
      }
      OS.flush();
      ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
      const auto Path = tmpFile("calls.c");
      std::ofstream(Path) << Source << R"(
uint64_t toy_callback(void *bytes) {
  uint64_t *state = (uint64_t *)bytes;
  ++state[4];
  return state[4] == state[2] ? 0x39 : 0;
}
int main(void) {
  for (uint64_t n = 1; n <= 32; ++n) {
    for (uint64_t stop = 0; stop <= n; ++stop) {
      uint64_t state[5] = {0, n, stop, 0, 0};
      uint64_t result = toy_caller((uint8_t *)state);
      if (result != (stop ? 0x39 : 0) ||
          state[0] != (stop ? 3 * stop - 2 : 3 * n) ||
          state[1] != (stop ? n - stop + 1 : 0) ||
          state[2] != stop || state[4] != (stop ? stop : n))
        return 1;
    }
  }
  return 0;
}
)";
      for (const char *Opt : {"-O0", "-O2"}) {
        const auto Program = tmpFile(std::string("calls-test") +
                                     neverd::test::executableSuffix());
        auto Built = exec(NEVERD_TEST_CLANG,
                          {"-std=c11", "-Werror", Opt, "-fsanitize=undefined",
                           "-fsanitize-trap=undefined", Path.string(), "-o",
                           Program.string()});
        ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
        auto Ran = exec(Program.string(), {});
        ASSERT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode << '\n'
                              << Source;
      }
    }
  }
}

TEST_P(BytecodeDecoderSourceTest,
       BothCRoutesPreserveLoopsNarrowWritesAndMemory) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "source runtime checks require clang";
  auto D = sourceDecoder(toyProfile(), GetParam());
  ASSERT_TRUE(bool(D));
  auto F = (*D)->function(ToyProgram, 0, 0, ToyProgram.size(), "toy_execute");
  ASSERT_TRUE(bool(F)) << llvm::toString(F.takeError());
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    auto Wrapped = lowerBytecodeState(F->Function, 32, A);
    ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
    std::map<va_t, SourceFunctionTypeHint> Hints{{0, Wrapped->SourceABI}};
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      SCOPED_TRACE(static_cast<unsigned>(A));
      LowToMedConverter Converter;
      Converter.setSourceCallHintsEnabled(true);
      Converter.setSourceEntryTypeHints(&Hints);
      auto Med = Converter.convert(Wrapped->Function, A);
      Med.SourceTypeHint = Wrapped->SourceABI;
      inferMedTypes(Med, A);
      ASSERT_TRUE(verifyMedFunc(Med, "bytecode-source"));
      ASSERT_EQ(Med.Params.size(), 1u);
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.Format = BinaryFormat::ELF;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      if (LLVM) {
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({Med}, Context, "bytecode-test", A);
        ASSERT_NE(Module, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
      } else {
        auto High = MedToHighConverter().convert(Med, A);
        ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      }
      OS.flush();
      const auto Path = tmpFile("bytecode.c");
      std::ofstream(Path) << Source << R"(
int main(void) {
  for (uint64_t n = 1; n <= 256; ++n) {
    uint64_t output[3] = {0x12345678, 0, 0xabcdef};
    uint64_t initial = (0xa57ef01934560000ULL ^ (n << 24)) | 0x9876;
    uint64_t state[4] = {initial, n, (uint64_t)(uintptr_t)&output[1],
                         0x123456789abcdeffULL};
    uint64_t expected = (initial & ~0xffffULL) | 0x1234;
    expected += n * (n + 1) / 2;
    if (toy_execute((uint8_t *)state) != 0 || state[0] != expected ||
        state[1] != 0 || state[3] != 0x123456789abcde00ULL ||
        output[1] != expected || output[0] != 0x12345678 || output[2] != 0xabcdef)
      return 1;
  }
  return 0;
}
)";
      for (const char *Opt : {"-O0", "-O2"}) {
        const auto Program = tmpFile(std::string("bytecode-test") +
                                     neverd::test::executableSuffix());
        auto Built =
            exec(NEVERD_TEST_CLANG, {"-std=c11", Opt, "-fsanitize=undefined",
                                     "-fsanitize-trap=undefined", Path.string(),
                                     "-o", Program.string()});
        ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
        auto Ran = exec(Program.string(), {});
        ASSERT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode << '\n'
                              << Source;
      }
    }
  }
}

} // namespace
