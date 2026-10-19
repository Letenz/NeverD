//===- X86_32_DebugParamTests.cpp - i386 debug parameters by location -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A pointer of an i386 DWARF unit is four bytes, its address size, so the
/// parameter after it arrives in the next stack slot.  Each recovered
/// parameter is named and typed after the source parameter there; read as
/// eight bytes, a pointer took two slots and every later name moved.  A
/// function whose DWARF says its parameters enter in registers (regparm)
/// takes those there and the rest in the slots after.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>

class X86_32_DebugParams : public NeverDLiftTest {
protected:
  std::string decompile(const char *ObjectName, const char *Output,
                        std::vector<std::string> Flags = {}) {
    const fs::path Object = fs::path(TEST_OBJ_DIR) / ObjectName;
    EXPECT_TRUE(fs::exists(Object)) << Object;
    const auto CFile = tmpFile(Output);
    std::vector<std::string> Args = {"decompile"};
    Args.insert(Args.end(), Flags.begin(), Flags.end());
    Args.insert(Args.end(), {"-o", CFile.string(), Object.string()});
    const auto R = exec(ndBin(), Args);
    EXPECT_EQ(R.exitCode, 0) << R.err;
    std::ifstream Ifs(CFile);
    EXPECT_TRUE(Ifs.good()) << CFile;
    return std::string((std::istreambuf_iterator<char>(Ifs)),
                       std::istreambuf_iterator<char>());
  }
};

TEST_F(X86_32_DebugParams, ParametersAfterAPointerTakeTheNextSlot) {
  const fs::path Object = fs::path(TEST_OBJ_DIR) / "test_debug_params32.o";
  ASSERT_TRUE(fs::exists(Object)) << Object;
  const auto CFile = tmpFile("debug_params32.c");
  const auto R =
      exec(ndBin(), {"decompile", "-o", CFile.string(), Object.string()});
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream Ifs(CFile);
  ASSERT_TRUE(Ifs.good()) << CFile;
  const std::string Source((std::istreambuf_iterator<char>(Ifs)),
                           std::istreambuf_iterator<char>());
  for (const char *Definition :
       {"int32_t dp32_sum(uint8_t* buf, int32_t len, int32_t bias)",
        "int32_t dp32_area(Window* w, int32_t scale)"})
    EXPECT_NE(Source.find(Definition), std::string::npos) << Definition << "\n"
                                                          << Source;
  EXPECT_EQ(Source.find("buf_4"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("w_4"), std::string::npos) << Source;
}

TEST_F(X86_32_DebugParams, RegisterParametersAreNamedWhereTheyEnter) {
  const std::string Source =
      decompile("test_debug_regparm32.o", "debug_regparm32.c");
  EXPECT_NE(Source.find("int32_t dp32_regs(uint8_t* buf, int32_t len, "
                        "int32_t bias, int32_t extra)"),
            std::string::npos)
      << Source;
  // The registers come in the order the code takes them, ECX before EDX
  // here, each named after the parameter that enters in it; placed as
  // cdecl's, the stack parameter took the first one's name.
  std::string Definition;
  for (size_t At = Source.find("int32_t dp32_skip(");
       At != std::string::npos && Definition.empty();
       At = Source.find("int32_t dp32_skip(", At + 1)) {
    const std::string Line = Source.substr(At, Source.find('\n', At) - At);
    if (Line.size() > 2 && Line.compare(Line.size() - 2, 2, " {") == 0)
      Definition = Line;
  }
  ASSERT_FALSE(Definition.empty()) << Source;
  EXPECT_NE(Definition.find("int32_t bias, int32_t len"), std::string::npos)
      << Definition;
  EXPECT_EQ(Definition.find("unused"), std::string::npos) << Definition;
  EXPECT_EQ(Definition.find("extra"), std::string::npos) << Definition;
}

TEST_F(X86_32_DebugParams, LLVMCNamesRegisterParametersWhereTheyEnter) {
  const std::string Source =
      decompile("test_debug_regparm32.o", "debug_regparm32_llvm.c", {"--llvm"});
  EXPECT_NE(Source.find("dp32_regs(uint8_t* buf, uint32_t len, uint32_t bias, "
                        "uint32_t extra)"),
            std::string::npos)
      << Source;
}
