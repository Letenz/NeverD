//===- X86InstructionCoverageTests.cpp - Real encoding coverage ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode the pinned Capstone x86 MC corpus and verify strict lifting or an
/// explicit architectural refusal. Count actual lifted encodings separately.
/// This does not claim complete ISA, emitter or executable semantic coverage.
//===----------------------------------------------------------------------===//

#include "../x86/X86LegacyAdmissionFixtures.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Lifter.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace neverd;

TEST(X86InstructionCoverage, RexRDoesNotCreateExtraDebugRegisters) {
  for (unsigned Index = 0; Index < 8; ++Index)
    for (bool Write : {false, true})
      for (bool ForLift : {false, true}) {
        const std::vector<uint8_t> Bytes = {0x44, 0x0f,
                                            uint8_t(Write ? 0x23 : 0x21),
                                            uint8_t(0xc0 + 8 * Index)};
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X64));
        DecodedInsn I{};
        const auto Length =
            ForLift ? D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I)
                    : D.decodeOne(Bytes.data(), Bytes.size(), 0x1000, I);
        if (!Length)
          continue;
        ASSERT_EQ(Length, Bytes.size());
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
}

TEST(X86InstructionCoverage, MpxRequiresOwnedBoundsStateAndFaultSemantics) {
  struct Fixture {
    std::vector<uint8_t> Bytes;
    unsigned Id;
  };
  const Fixture Cases[] = {
      {{0xf3, 0x0f, 0x1a, 0xc0}, X86_INS_BNDCL},
      {{0xf3, 0x0f, 0x1a, 0x00}, X86_INS_BNDCL},
      {{0xf2, 0x0f, 0x1a, 0xc0}, X86_INS_BNDCU},
      {{0xf2, 0x0f, 0x1a, 0x00}, X86_INS_BNDCU},
      {{0xf2, 0x0f, 0x1b, 0xc0}, X86_INS_BNDCN},
      {{0xf2, 0x0f, 0x1b, 0x00}, X86_INS_BNDCN},
      {{0xf3, 0x0f, 0x1b, 0x00}, X86_INS_BNDMK},
      {{0x66, 0x0f, 0x1a, 0xc1}, X86_INS_BNDMOV},
      {{0x66, 0x0f, 0x1a, 0x00}, X86_INS_BNDMOV},
      {{0x66, 0x0f, 0x1b, 0x00}, X86_INS_BNDMOV},
      {{0x0f, 0x1a, 0x04, 0x08}, X86_INS_BNDLDX},
      {{0x0f, 0x1b, 0x04, 0x08}, X86_INS_BNDSTX},
  };
  for (auto A : {Arch::X86, Arch::X64})
    for (const auto &F : Cases)
      for (unsigned Register = 0; Register != 4; ++Register)
        for (bool ForLift : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(F.Id);
          SCOPED_TRACE(Register);
          SCOPED_TRACE(ForLift);
          auto Bytes = F.Bytes;
          Bytes[Bytes[0] == 0x0f ? 2 : 3] |= Register << 3;
          Decoder D;
          ASSERT_TRUE(D.init(A));
          DecodedInsn I{};
          ASSERT_EQ(
              ForLift
                  ? D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I)
                  : D.decodeOne(Bytes.data(), Bytes.size(), 0x1000, I),
              Bytes.size());
          ASSERT_EQ(I.Id, F.Id);
          std::vector<LowOp> Ops;
          EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
}

TEST(X86InstructionCoverage, PinnedDecoderCorpusHasExplicitLiftResults) {
  const std::filesystem::path Directory = NEVERD_X86_MC_DIRECTORY;
  ASSERT_TRUE(std::filesystem::is_directory(Directory));
  size_t Samples = 0;
  size_t Refused = 0;
  std::set<unsigned> Ids;
  for (const auto &File : std::filesystem::directory_iterator(Directory)) {
    if (File.path().extension() != ".cs")
      continue;
    std::ifstream Input(File.path());
    ASSERT_TRUE(Input);
    std::string Line;
    ASSERT_TRUE(std::getline(Input, Line));
    Arch A = Line.find("CS_MODE_64") != std::string::npos   ? Arch::X64
             : Line.find("CS_MODE_32") != std::string::npos ? Arch::X86
                                                            : Arch::Unknown;
    if (A == Arch::Unknown)
      continue; // 16-bit mode has no NeverD target.
    Decoder D;
    ASSERT_TRUE(D.init(A));
    unsigned Number = 1;
    while (std::getline(Input, Line)) {
      ++Number;
      if (!Line.starts_with("0x"))
        continue;
      const auto Equal = Line.find('=');
      ASSERT_NE(Equal, std::string::npos);
      std::string Encoding = Line.substr(0, Equal);
      std::replace(Encoding.begin(), Encoding.end(), ',', ' ');
      std::istringstream Hex(Encoding);
      std::vector<uint8_t> Bytes;
      unsigned Byte;
      while (Hex >> std::hex >> Byte) {
        ASSERT_LE(Byte, 255u);
        Bytes.push_back(Byte);
      }
      SCOPED_TRACE(File.path().filename().string() + ":" +
                   std::to_string(Number));
      SCOPED_TRACE(Line);
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
                Bytes.size());
      std::vector<LowOp> Ops;
      const bool LegacyGap =
          std::ranges::any_of(test::X86FarPointerEncodings,
                              [&](const auto &E) { return E.Id == I.Id; }) ||
          std::ranges::any_of(test::X86ThreeDNowEncodings,
                              [&](const auto &E) { return E.Id == I.Id; });
      // Far and interrupt returns (RETF/RETFQ/IRET/IRETD/IRETQ) now lift to a
      // faithful terminal state-transfer intrinsic, so they take the ordinary
      // lift path below.  The remaining domain-leaving instructions still have
      // no faithful ordinary-C projection and are refused.
      if (I.Id == X86_INS_SYSENTER || I.Id == X86_INS_SYSEXIT ||
          I.Id == X86_INS_SYSEXITQ || I.Id == X86_INS_SYSRET ||
          I.Id == X86_INS_SYSRETQ || I.Id == X86_INS_RSM || LegacyGap) {
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        ++Refused;
        continue;
      }
      EXPECT_NO_THROW(D.liftToLow(I, Ops));
      Ids.insert(I.Id);
      ++Samples;
    }
  }
  EXPECT_GT(Samples, 0u);
  RecordProperty("encoding_samples", Samples);
  RecordProperty("instruction_ids", Ids.size());
  RecordProperty("explicitly_refused_samples", Refused);
}
