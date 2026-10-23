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

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace neverd;

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
      if (I.Id == X86_INS_RETF || I.Id == X86_INS_RETFQ ||
          I.Id == X86_INS_IRET || I.Id == X86_INS_IRETD ||
          I.Id == X86_INS_IRETQ) {
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
