//===- X86_64_DebugRecordTests.cpp - Debug records C cannot spell ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// DWARF can describe a record C has no layout for, such as one with bit
/// fields.  Its pointers, values, results and arrays keep the machine types
/// the decompiler recovered, in the whole program and one function at a
/// time, instead of ending the decompile.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>

class X86_64_DebugRecords : public NeverDLiftTest {};

static fs::path debugRecordsObj() {
  return fs::path(TEST_OBJ_DIR) / "test_debug_records.o";
}

TEST_F(X86_64_DebugRecords, RecordsCCannotSpellKeepMachineTypes) {
  ASSERT_TRUE(fs::exists(debugRecordsObj())) << debugRecordsObj();
  const auto CFile = tmpFile("debug_records.c");
  const auto R = exec(
      ndBin(), {"decompile", "-o", CFile.string(), debugRecordsObj().string()});
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream Ifs(CFile);
  ASSERT_TRUE(Ifs.good()) << CFile;
  const std::string Source((std::istreambuf_iterator<char>(Ifs)),
                           std::istreambuf_iterator<char>());
  for (const char *Definition :
       {"flags_sum(int64_t f)", "flags_mode(int64_t f)", "flags_walk("})
    EXPECT_NE(Source.find(Definition), std::string::npos) << Source;
  // The returned record keeps the recovered result type, and says why.
  EXPECT_NE(Source.find("return=(no C type)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("nd_record"), std::string::npos) << Source;

  for (const char *Function : {"flags_walk", "flags_make", "flags_mode"}) {
    SCOPED_TRACE(Function);
    const auto One = exec(
        ndBin(), {"decompile", "--func", Function, debugRecordsObj().string()});
    EXPECT_EQ(One.exitCode, 0) << One.err;
    EXPECT_NE(One.out.find(Function), std::string::npos) << One.out;
  }
}
