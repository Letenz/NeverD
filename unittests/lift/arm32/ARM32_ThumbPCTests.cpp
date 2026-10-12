//===- ARM32_ThumbPCTests.cpp - T32 reads of PC ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// T32 reads PC as an instruction's address plus 4, and a literal access (LDR
// literal, ADR) adds its offset to that value aligned down to a word.  The two
// differ for an instruction at the second halfword of a word, where each case
// in test_thumb_pc_reads.s sits.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

#include "llvm/ADT/StringExtras.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace {

fs::path thumbPCObj() {
  return fs::path(TEST_OBJ_DIR) / "test_thumb_pc_reads.o";
}

/// How the LowIR dump prints \p Value.
std::string hex(uint64_t Value) { return "0x" + llvm::utohexstr(Value); }

/// The dump of function \p Name in LowIR dump \p Low, and its address.
std::pair<std::string, uint64_t> lowFunction(const std::string &Low,
                                             const std::string &Name) {
  const std::string Header = "func " + Name + " @ 0x";
  const size_t Start = Low.find(Header);
  if (Start == std::string::npos)
    return {"", 0};
  const size_t AddrStart = Start + Header.size();
  const uint64_t Addr = std::stoull(
      Low.substr(AddrStart, Low.find(' ', AddrStart) - AddrStart), nullptr, 16);
  const size_t End = Low.find("\nfunc ", AddrStart);
  return {Low.substr(Start, End == std::string::npos ? End : End - Start),
          Addr};
}

/// The body of function \p Name in HighC source \p C.
std::string highCFunction(const std::string &C, const std::string &Name) {
  const size_t Start = C.find(" " + Name + "(");
  if (Start == std::string::npos)
    return "";
  return C.substr(Start, C.find("\n}", Start) - Start);
}

} // namespace

class ARM32_ThumbPC : public HighCHostExecutionTest {
protected:
  ARM32_ThumbPC() { HostFlags = {"-m32"}; }
};

TEST_F(ARM32_ThumbPC, AddReadsTheInstructionAddressPlusFour) {
  const auto R = liftToLowIR(thumbPCObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const auto [Body, F] = lowFunction(R.out, "thumb_add_pc");
  ASSERT_FALSE(Body.empty()) << R.out;
  // `add r0, pc` at F+2.
  EXPECT_NE(Body.find("[" + hex(F + 2) +
                      ".0] COPY reg:0x3C:4 cst:" + hex(F + 6) + ":4"),
            std::string::npos)
      << Body;
}

TEST_F(ARM32_ThumbPC, AdrAddsItsOffsetToTheAlignedPC) {
  const auto R = liftToLowIR(thumbPCObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const auto [Body, F] = lowFunction(R.out, "thumb_adr_forms");
  ASSERT_FALSE(Body.empty()) << R.out;
  // The two forward forms name the word after the function, the subtracting
  // one the word before it.
  EXPECT_NE(Body.find("COPY reg:0x0:4 cst:" + hex(F + 16) + ":4"),
            std::string::npos)
      << Body;
  EXPECT_NE(Body.find("COPY reg:0x4:4 cst:" + hex(F + 16) + ":4"),
            std::string::npos)
      << Body;
  EXPECT_NE(Body.find("COPY reg:0x8:4 cst:" + hex(F - 4) + ":4"),
            std::string::npos)
      << Body;
}

TEST_F(ARM32_ThumbPC, LiteralLoadReadsFromTheAlignedPC) {
  const auto R = liftToLowIR(thumbPCObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const auto [Body, F] = lowFunction(R.out, "thumb_literal_load");
  ASSERT_FALSE(Body.empty()) << R.out;
  // `ldr r0, [pc, #4]` at F+2 adds its offset to F+4.
  EXPECT_NE(Body.find("[" + hex(F + 2) +
                      ".0] COPY reg:0x3C:4 cst:" + hex(F + 4) + ":4"),
            std::string::npos)
      << Body;
}

// A literal added to PC names the object it addresses, both an R_ARM_REL32
// literal and one the assembler resolved within its section.
TEST_F(ARM32_ThumbPC, PCRelativeLiteralsNameWhatTheyAddress) {
  const auto R = decompileToHighC(thumbPCObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream IFS(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(IFS) << "C output file not created";
  std::ostringstream SS;
  SS << IFS.rdbuf();
  const std::string C = SS.str();
  const std::string Data = highCFunction(C, "thumb_pic_data");
  ASSERT_FALSE(Data.empty()) << C;
  EXPECT_NE(Data.find("thumb_pic_target"), std::string::npos) << Data;
  const std::string Code = highCFunction(C, "thumb_local_fp");
  ASSERT_FALSE(Code.empty()) << C;
  EXPECT_NE(Code.find("thumb_fp_target"), std::string::npos) << Code;
}

// Position-independent T32 code reaches its static data through literals
// added to PC.
TEST_F(ARM32_ThumbPC, PICDataRunsLikeTheSource) {
  expectHighCRunsLikeSource(
      {"-target", "armv7-linux-gnueabi", "-mthumb", "-fPIC"}, R"C(
static int thumb_table[8];
static const int thumb_weights[4] = {3, 5, 7, 11};
int thumb_mix(int a) {
  thumb_table[a & 7] = a;
  thumb_table[(a + 3) & 7] += a * 3;
  return thumb_table[(a + 1) & 7] + thumb_weights[a & 3] + thumb_table[a & 7];
}
)C",
      "thumb_mix", {"thumb_mix", "thumb_table", "thumb_weights"},
      {0, 1, -1, 0x15, 0x27, 0x3F, 0x12345678});
}
