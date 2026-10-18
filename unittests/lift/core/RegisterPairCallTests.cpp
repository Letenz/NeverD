//===- RegisterPairCallTests.cpp - Pairs callers take in part or pass on --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A 32-bit function returns a 64-bit integer in r0:r1 (ARM) or edx:eax
// (i386).  A caller that reads only the high half, or passes the pair on to
// the next call, shows the callee's contract, which the callee's body confirms
// (test_register_pair_returns_*.s).  A callee that leaves the high register
// alone returns no pair, whatever its callers read after it.
//
//===----------------------------------------------------------------------===//

#include "CSourceText.h"
#include "NeverDLiftFixture.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace {

std::string readText(const fs::path &Path) {
  std::ifstream In(Path);
  std::ostringstream Text;
  Text << In.rdbuf();
  return Text.str();
}

/// The definition of \p Name in C source \p C, from its return type.
std::string definition(const std::string &C, const std::string &Name) {
  const size_t At = neverd::test::cDefinitionAt(C, Name);
  if (At == std::string::npos)
    return {};
  const size_t Begin = C.rfind('\n', At) + 1;
  return C.substr(Begin, C.find("\n}", At) - Begin);
}

bool returnsPair(const std::string &Definition) {
  return Definition.rfind("int64_t ", 0) == 0 ||
         Definition.rfind("uint64_t ", 0) == 0;
}

} // namespace

class RegisterPairCall : public NeverDLiftTest {
protected:
  /// The unit a C route writes for \p Object.
  std::string source(const fs::path &Object, bool LLVM) {
    const auto R = LLVM ? decompileToC(Object) : decompileToHighC(Object);
    EXPECT_EQ(R.exitCode, 0) << R.err;
    return readText(tmpFile(LLVM ? "decompiled.c" : "decompiled_high.c"));
  }
};

TEST_F(RegisterPairCall, CallersShowTheCalleesPair) {
  const std::pair<const char *, const char *> Fixtures[] = {
      {"test_register_pair_returns_arm.o", "pair_read_tp"},
      {"test_register_pair_returns_i386.o", "pair_keeps_edx_callee"}};
  for (const auto &[Fixture, Keeper] : Fixtures)
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(std::string(Fixture) + (LLVM ? " LLVM C" : " HighC"));
      const std::string C = source(fs::path(TEST_OBJ_DIR) / Fixture, LLVM);
      // The high half alone (pair_high's call), and the pair passed on to the
      // next call (pair_sum3's first).
      for (const char *Name : {"pair_wide", "pair_add"})
        EXPECT_TRUE(returnsPair(definition(C, Name))) << Name << "\n" << C;
      const std::string High = definition(C, "pair_high");
      EXPECT_FALSE(returnsPair(High)) << C;
      EXPECT_NE(High.rfind("void ", 0), 0u) << C;
      EXPECT_NE(High.find("pair_wide("), std::string::npos) << C;
      EXPECT_NE(High.find(">> "), std::string::npos) << C;
      EXPECT_EQ(definition(C, "pair_sum3").find("__builtin_trap"),
                std::string::npos)
          << C;
      // A caller's read of the high register after a callee that leaves it
      // alone shows no pair.
      EXPECT_FALSE(returnsPair(definition(C, Keeper))) << C;
    }
}
