//===- X86_64_UndefinedEffectsTests.cpp - arbitrary-result metadata -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowUndefinedEffects.h"
#include "neverd/lift/X86Regs.h"

#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

struct InstructionCase {
  const char *Name;
  std::vector<uint8_t> Bytes;
};

class X86UndefinedEffects : public testing::Test {
protected:
  Decoder Dec;

  void SetUp() override { ASSERT_TRUE(Dec.init(Arch::X64)); }

  bool decode(const std::vector<uint8_t> &Bytes, DecodedInsn &Insn) {
    return Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn) ==
           static_cast<int>(Bytes.size());
  }
};

void expectSameOps(llvm::ArrayRef<LowOp> Expected,
                   llvm::ArrayRef<LowOp> Actual) {
  ASSERT_EQ(Expected.size(), Actual.size());
  for (size_t I = 0; I < Expected.size(); ++I) {
    SCOPED_TRACE(I);
    const LowOp &A = Expected[I];
    const LowOp &B = Actual[I];
    EXPECT_EQ(A.Opcode, B.Opcode);
    EXPECT_EQ(A.MemoryOrdering, B.MemoryOrdering);
    EXPECT_EQ(A.MemoryAddressSpace, B.MemoryAddressSpace);
    EXPECT_EQ(A.Output, B.Output);
    EXPECT_EQ(A.Addr, B.Addr);
    EXPECT_EQ(A.Seq, B.Seq);
    ASSERT_EQ(A.NumInputs, B.NumInputs);
    for (uint8_t J = 0; J < A.NumInputs; ++J)
      EXPECT_EQ(A.Inputs[J], B.Inputs[J]);
  }
}

LowInstructionUndefinedEffects staleEffects() {
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OpCount = 99;
  Effects.OperationDigest = "stale operation digest";
  Effects.Effects.push_back(
      {99, NdVar::reg(x86reg::AF, 1), 0, 1, NdVar::scalar(1, 1)});
  Effects.Diagnostic = "stale certificate";
  return Effects;
}

void expectCleared(const LowInstructionUndefinedEffects &Effects) {
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_NE(Effects.OpCount, 99U);
  EXPECT_NE(Effects.OperationDigest, "stale operation digest");
  EXPECT_NE(Effects.Diagnostic, "stale certificate");
}

TEST_F(X86UndefinedEffects, LogicalInstructionsProduceOnlyAuxiliaryCarryBit) {
  const InstructionCase Cases[] = {
      {"and eax, ecx", {0x21, 0xc8}}, {"or eax, ecx", {0x09, 0xc8}},
      {"xor eax, ecx", {0x31, 0xc8}}, {"test eax, ecx", {0x85, 0xc8}},
      {"xor eax, eax", {0x31, 0xc0}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> PlainOps, CertifiedOps;
    Dec.liftToLow(Insn, PlainOps);
    LowInstructionUndefinedEffects Effects;
    Dec.liftToLow(Insn, CertifiedOps, {}, {}, &Effects);
    expectSameOps(PlainOps, CertifiedOps);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
    EXPECT_EQ(Effects.OpCount, CertifiedOps.size());
    EXPECT_EQ(Effects.OperationDigest,
              lowUndefinedOperationDigest(CertifiedOps));
    ASSERT_EQ(Effects.Effects.size(), 1U);
    const LowUndefinedEffect &Effect = Effects.Effects.front();
    EXPECT_EQ(Effect.Output, NdVar::reg(x86reg::AF, 1));
    EXPECT_EQ(Effect.BitOffset, 0U);
    EXPECT_EQ(Effect.BitCount, 1U);
    EXPECT_FALSE(Effect.When.has_value());
    EXPECT_GT(Effect.AfterOp, 0U);
    EXPECT_LE(Effect.AfterOp, Effects.OpCount);
  }
}

TEST_F(X86UndefinedEffects, DefinedAndPreservedFlagsCreateNoArbitraryProducer) {
  const InstructionCase Cases[] = {
      {"add eax, ecx", {0x01, 0xc8}},
      {"adc eax, ecx", {0x11, 0xc8}},
      {"sub eax, ecx", {0x29, 0xc8}},
      {"sbb eax, ecx", {0x19, 0xc8}},
      {"cmp eax, ecx", {0x39, 0xc8}},
      {"mov eax, ecx", {0x89, 0xc8}},
      {"nop", {0x90}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> PlainOps, CertifiedOps;
    Dec.liftToLow(Insn, PlainOps);
    auto Effects = staleEffects();
    Dec.liftToLow(Insn, CertifiedOps, {}, {}, &Effects);
    expectSameOps(PlainOps, CertifiedOps);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
    EXPECT_EQ(Effects.OpCount, CertifiedOps.size());
    EXPECT_EQ(Effects.OperationDigest,
              lowUndefinedOperationDigest(CertifiedOps));
    EXPECT_TRUE(Effects.Effects.empty());
    EXPECT_NE(Effects.Diagnostic, "stale certificate");
  }
}

TEST_F(X86UndefinedEffects,
       BoundariesExcludePrefixOpsAndIncludeFinalZeroExtend) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn)); // xor eax, eax
  std::vector<LowOp> StandaloneOps;
  LowInstructionUndefinedEffects Standalone;
  Dec.liftToLow(Insn, StandaloneOps, {}, {}, &Standalone);
  ASSERT_FALSE(StandaloneOps.empty());
  const LowOp &Last = StandaloneOps.back();
  EXPECT_EQ(Last.Opcode, NdOp::INT_ZEXT);
  EXPECT_EQ(Last.Output, NdVar::reg(x86reg::RAX, 8));
  ASSERT_EQ(Last.NumInputs, 1U);
  EXPECT_EQ(Last.Inputs[0], NdVar::reg(x86reg::RAX, 4));

  LowOp Prefix;
  Prefix.Opcode = NdOp::NOP;
  Prefix.Addr = 0x800;
  const std::vector<LowOp> PrefixOps(3, Prefix);
  auto AppendedOps = PrefixOps;
  LowInstructionUndefinedEffects Appended;
  Dec.liftToLow(Insn, AppendedOps, {}, {}, &Appended);
  expectSameOps(
      PrefixOps,
      llvm::ArrayRef<LowOp>(AppendedOps).take_front(PrefixOps.size()));
  expectSameOps(
      StandaloneOps,
      llvm::ArrayRef<LowOp>(AppendedOps).drop_front(PrefixOps.size()));
  EXPECT_EQ(Appended.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_EQ(Appended.OpCount, StandaloneOps.size());
  EXPECT_EQ(Appended.OperationDigest, Standalone.OperationDigest);
  EXPECT_EQ(
      Appended.OperationDigest,
      lowUndefinedOperationDigest(
          llvm::ArrayRef<LowOp>(AppendedOps).drop_front(PrefixOps.size())));
  ASSERT_EQ(Standalone.Effects.size(), 1U);
  ASSERT_EQ(Appended.Effects.size(), 1U);
  EXPECT_EQ(Appended.Effects.front().AfterOp,
            Standalone.Effects.front().AfterOp);
  EXPECT_LE(Appended.Effects.front().AfterOp, Appended.OpCount);
}

TEST_F(X86UndefinedEffects, UnauditedInstructionsDoNotClaimCompleteCoverage) {
  const InstructionCase Cases[] = {
      {"rol eax, cl", {0xd3, 0xc0}},
      {"bsf eax, ecx", {0x0f, 0xbc, 0xc1}},
      {"apx inc ndd nf", {0x62, 0xec, 0xf5, 0x14, 0xff, 0xc3}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
  }
}

TEST_F(X86UndefinedEffects, LegacyShiftCountsPublishCompleteEvidence) {
  const InstructionCase Cases[] = {
      {"shl al, cl", {0xd2, 0xe0}},
      {"shr ax, cl", {0x66, 0xd3, 0xe8}},
      {"sar eax, cl", {0xd3, 0xf8}},
      {"shl rcx, cl", {0x48, 0xd3, 0xe1}},
      {"shr ah, cl", {0xd2, 0xec}},
      {"sar byte [rbx], cl", {0xd2, 0x3b}},
      {"shl eax, 0", {0xc1, 0xe0, 0}},
      {"shr ax, 16", {0x66, 0xc1, 0xe8, 16}},
      {"sar rax, 1", {0x48, 0xd1, 0xf8}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> PlainOps, CertifiedOps;
    Dec.liftToLow(Insn, PlainOps);
    auto Effects = staleEffects();
    Dec.liftToLow(Insn, CertifiedOps, {}, {}, &Effects);
    expectSameOps(PlainOps, CertifiedOps);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Effects.Diagnostic;
    EXPECT_EQ(Effects.OpCount, CertifiedOps.size());
    EXPECT_EQ(Effects.OperationDigest,
              lowUndefinedOperationDigest(CertifiedOps));
  }
}

TEST_F(X86UndefinedEffects, ShiftUnauditedPrefixesClearAllPartialEvidence) {
  const InstructionCase Cases[] = {
      {"duplicate operand prefix", {0x66, 0x66, 0xd3, 0xe0}},
      {"REP shift", {0xf3, 0xd3, 0xe0}},
      {"REPNZ shift", {0xf2, 0xd3, 0xe0}},
      {"unused REX.R opcode extension", {0x44, 0xd3, 0xe0}},
      {"undocumented /6 alias", {0xd3, 0xf0}},
      {"BMI2 SHLX", {0xc4, 0xe2, 0x71, 0xf7, 0xc0}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    if (!decode(Case.Bytes, Insn))
      continue; // Decoder refusal is also an explicit boundary.
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  }
}

TEST_F(X86UndefinedEffects, ShiftMutatedCountWidthAndOpcodeRemainUnaudited) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode({0xd3, 0xe0}, Insn)); // shl eax,cl.
    auto &X = Insn.Raw->detail->x86;
    switch (Mutation) {
    case 0:
      X.operands[1].reg = X86_REG_DL;
      break;
    case 1:
      X.operands[1].reg = X86_REG_ECX;
      X.operands[1].size = 4;
      break;
    case 2:
      X.operands[0].reg = X86_REG_AX;
      X.operands[0].size = 2;
      break;
    case 3:
      X.operands[0].reg = X86_REG_ECX;
      break;
    case 4:
      Insn.Raw->bytes[1] = 0xe8;
      break; // Actual group is SHR.
    case 5:
      X.encoding.modrm_offset = 0;
      break;
    case 6:
      X.op_count = 1;
      break;
    case 7:
      X.operands[1].type = X86_OP_IMM;
      X.operands[1].imm = 1;
      break;
    }
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
  }
  DecodedInsn Immediate{};
  ASSERT_TRUE(decode({0xc1, 0xe0, 2}, Immediate));
  Immediate.Raw->detail->x86.operands[1].imm = 1;
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  Dec.liftToLow(Immediate, Ops, {}, {}, &Effects);
  expectCleared(Effects);
}

TEST_F(X86UndefinedEffects, PermissiveUnsupportedFallbackClearsOldCertificate) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x90}, Insn));
  Insn.Raw->id = X86_INS_INVALID;
  Dec.setStrict(false);
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Unsupported);
  expectCleared(Effects);
  ASSERT_EQ(Ops.size(), 1U);
  EXPECT_EQ(Ops.front().Opcode, NdOp::NOP);
  EXPECT_EQ(Effects.OpCount, Ops.size());
}

TEST_F(X86UndefinedEffects, StrictFailureClearsOldCertificateBeforeThrowing) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x90}, Insn));
  Insn.Raw->id = X86_INS_INVALID;
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects), UnliftedInstruction);
  expectCleared(Effects);
}

TEST_F(X86UndefinedEffects, MissingOperandDetailClearsOldCertificate) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn));
  cs_insn WithoutDetail = *Insn.Raw;
  WithoutDetail.detail = nullptr;
  Insn.Raw = &WithoutDetail;
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  expectCleared(Effects);
  EXPECT_EQ(Effects.OpCount, 0U);
  EXPECT_TRUE(Ops.empty());
}

TEST_F(X86UndefinedEffects,
       MemoryCallProjectionClearsOldCertificateOnRejection) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn));
  LowOp Prefix;
  Prefix.Addr = 0x800;
  std::vector<LowOp> Ops{Prefix};
  auto Effects = staleEffects();
  EXPECT_FALSE(Dec.liftX64MemoryCallToLow(Insn, Ops, &Effects));
  expectCleared(Effects);
  EXPECT_EQ(Effects.OpCount, 0U);
  expectSameOps(std::vector<LowOp>{Prefix}, Ops);
}

TEST_F(X86UndefinedEffects, MemoryCallProjectionDoesNotReuseLogicalEffects) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0xff, 0x10}, Insn)); // call qword ptr [rax]
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  EXPECT_TRUE(Dec.liftX64MemoryCallToLow(Insn, Ops, &Effects));
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_NE(Effects.Diagnostic, "stale certificate");
  EXPECT_FALSE(Ops.empty());
}

TEST_F(X86UndefinedEffects, MalformedOperandCountCannotCertifyEmptyLift) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn));
  ASSERT_NE(Insn.Raw->detail, nullptr);
  Insn.Raw->detail->x86.op_count = 1;
  Dec.setStrict(false);
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  expectCleared(Effects);
}

TEST_F(X86UndefinedEffects, MalformedWidthsAndAddressesDoNotCertifyShapes) {
  struct MutationCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
    void (*Mutate)(cs_x86 &);
  };
  const MutationCase Cases[] = {
      {"byte stack push",
       {0x6a, 0x01},
       [](cs_x86 &X) { X.operands[0].size = 1; }},
      {"byte lea",
       {0x8d, 0x01},
       [](cs_x86 &X) {
         X.operands[0].reg = X86_REG_AL;
         X.operands[0].size = 1;
       }},
      {"byte near call",
       {0xff, 0xd0},
       [](cs_x86 &X) {
         X.operands[0].reg = X86_REG_AL;
         X.operands[0].size = 1;
       }},
      {"byte near jump",
       {0xff, 0xe0},
       [](cs_x86 &X) {
         X.operands[0].reg = X86_REG_AL;
         X.operands[0].size = 1;
       }},
      {"byte movzx mutated to a dword source",
       {0x0f, 0xb6, 0xc1},
       [](cs_x86 &X) {
         X.operands[1].reg = X86_REG_ECX;
         X.operands[1].size = 4;
       }},
      {"byte movsx mutated to a dword source",
       {0x0f, 0xbe, 0xc1},
       [](cs_x86 &X) {
         X.operands[1].reg = X86_REG_ECX;
         X.operands[1].size = 4;
       }},
      {"invalid scale",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.operands[1].mem.scale = 3; }},
      {"invalid address width",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.addr_size = 3; }},
      {"vector base",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.operands[1].mem.base = X86_REG_XMM0; }},
      {"vector index",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.operands[1].mem.index = X86_REG_XMM0; }},
      {"invalid segment",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.operands[1].mem.segment = X86_REG_RAX; }},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> Baseline;
    LowInstructionUndefinedEffects Original;
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Baseline, {}, {}, &Original));
    ASSERT_EQ(Original.Coverage, LowUndefinedCoverage::Complete);
    Case.Mutate(Insn.Raw->detail->x86);
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
    EXPECT_FALSE(Effects.Diagnostic.empty());
  }
}

TEST_F(X86UndefinedEffects, MissingCoverageDoesNotPublishPartialEffects) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn));
  // Logical AF is locally recorded even when the complete transaction cannot
  // be certified. Do not expose it as a usable partial architecture contract.
  Dec.setStrict(false);
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_FALSE(Effects.Diagnostic.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
}

TEST(X86UndefinedEffectsForms, LegacyAliasesAndAddressWidthsRemainAudited) {
  struct FormCase {
    const char *Name;
    Arch Target;
    std::vector<uint8_t> Bytes;
  };
  const FormCase Cases[] = {
      {"high byte alias", Arch::X64, {0x88, 0xe0}},
      {"REX low byte", Arch::X64, {0x40, 0x88, 0xcc}},
      {"high byte sign extension", Arch::X64, {0x0f, 0xbe, 0xc5}},
      {"32-bit address in long mode", Arch::X64, {0x67, 0x8b, 0x04, 0x4b}},
      {"16-bit address, word data", Arch::X86, {0x67, 0x66, 0x8b, 0x00}},
      {"16-bit address, dword data", Arch::X86, {0x67, 0x8b, 0x00}},
      {"MOVSXD wide", Arch::X64, {0x48, 0x63, 0xc1}},
      {"immediate byte push", Arch::X64, {0x6a, 0x01}},
      {"word push", Arch::X64, {0x66, 0x50}},
      {"immediate ALU", Arch::X64, {0x48, 0x83, 0xc0, 0x01}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Case.Target));
    DecodedInsn Insn{};
    ASSERT_EQ(Decode.decodeOneForLift(Case.Bytes.data(), Case.Bytes.size(),
                                      0x1000, Insn),
              static_cast<int>(Case.Bytes.size()));
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Insn.Raw->mnemonic << " " << Insn.Raw->op_str << ": "
        << Effects.Diagnostic;
    EXPECT_TRUE(Effects.Effects.empty());
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  }
}

TEST_F(X86UndefinedEffects, PinnedDecoderRefusesNarrowOpcode63Forms) {
  // This is an existing decoder coverage gap, not successful metadata
  // coverage. Keep it explicit until the pinned decoder supports these forms.
  const InstructionCase Cases[] = {
      {"opcode 63 without REX.W", {0x63, 0xc1}},
      {"opcode 63 word form", {0x66, 0x63, 0xc1}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    EXPECT_EQ(Dec.decodeOneForLift(Case.Bytes.data(), Case.Bytes.size(), 0x1000,
                                   Insn),
              0);
  }
}

TEST_F(X86UndefinedEffects, DecodedEqualWidthExtensionsRemainUnaudited) {
  // Unlike the mutated dword-source operands above, these equal-width
  // 16-bit forms really decode. They remain outside the initial flag audit.
  const InstructionCase Cases[] = {
      {"word movsx", {0x66, 0x0f, 0xbf, 0xc1}},
      {"word movzx", {0x66, 0x0f, 0xb7, 0xc1}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    ASSERT_EQ(Insn.Raw->detail->x86.op_count, 2U);
    ASSERT_EQ(Insn.Raw->detail->x86.operands[0].size, 2U);
    ASSERT_EQ(Insn.Raw->detail->x86.operands[1].size, 2U);
    std::vector<LowOp> PlainOps, CertifiedOps;
    ASSERT_NO_THROW(Dec.liftToLow(Insn, PlainOps));
    auto Effects = staleEffects();
    ASSERT_NO_THROW(Dec.liftToLow(Insn, CertifiedOps, {}, {}, &Effects));
    expectSameOps(PlainOps, CertifiedOps);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
    EXPECT_FALSE(Effects.Diagnostic.empty());
    EXPECT_EQ(Effects.OpCount, CertifiedOps.size());
    EXPECT_EQ(Effects.OperationDigest,
              lowUndefinedOperationDigest(CertifiedOps));
  }
}

TEST(X86UndefinedEffectsOtherArch, AArch64DoesNotReuseAnX86Certificate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  const uint8_t Bytes[] = {0x1f, 0x20, 0x03, 0xd5}; // nop
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), 0x1000, Insn), 4);
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
  expectCleared(Effects);
}

TEST(X86UndefinedEffectsForms, ShiftAddressOverridesKeepCountEvidence) {
  for (const auto &[Target, Bytes] :
       std::vector<std::pair<Arch, std::vector<uint8_t>>>{
           {Arch::X86, {0x67, 0x66, 0xd3, 0x20}},
           {Arch::X86, {0x67, 0xd3, 0x20}},
           {Arch::X64, {0x67, 0xd3, 0x64, 0x8b, 4}},
           {Arch::X64, {0x64, 0x48, 0xd3, 0x20}}}) {
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    DecodedInsn Insn{};
    ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              static_cast<int>(Bytes.size()));
    std::vector<LowOp> Plain, Certified;
    LowInstructionUndefinedEffects Effects;
    Decode.liftToLow(Insn, Plain);
    Decode.liftToLow(Insn, Certified, {}, {}, &Effects);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Effects.Diagnostic;
    expectSameOps(Plain, Certified);
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Certified));
    ASSERT_GE(Effects.Effects.size(), 2u);
    for (const auto &Effect : Effects.Effects)
      EXPECT_TRUE(Effect.When.has_value());
  }
}

} // namespace
