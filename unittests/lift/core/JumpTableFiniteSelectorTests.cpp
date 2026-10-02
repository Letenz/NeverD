//===- JumpTableFiniteSelectorTests.cpp - Exact finite selectors ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"

#include <iterator>
#include <optional>

namespace {

class JumpTableFiniteSelector : public NeverDLiftTest {
protected:
  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  fs::path compileFixture(const std::string &Arch, unsigned Case) {
    const fs::path Source =
        fs::path(TEST_SOURCE_DIR) /
        (Arch == "aarch64" ? "aarch64/test_jumptable_finite_selector_a64.S"
                           : "x86_64/test_jumptable_finite_selector.S");
    const fs::path Object = tmpFile(Arch + "-" + std::to_string(Case) + ".o");
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", Arch + "-linux-gnu",
                                 "-DTEST_CASE=" + std::to_string(Case), "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  static neverd::LowFunc build(const neverd::BinaryImage &Image,
                               const std::string &Name,
                               std::optional<size_t> Budget = std::nullopt) {
    const auto *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr);
    if (!Function)
      return {};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    if (Budget)
      Builder.setMaskFixedPointEvidenceBudgetForTesting(*Budget);
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  }

  static bool hasOpcode(const neverd::LowFunc &Function, neverd::NdOp Opcode) {
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == Opcode)
          return true;
    return false;
  }
};

TEST_F(JumpTableFiniteSelector, SparseSelectionPublishesOnlyFeasibleSlots) {
  for (const std::string Arch : {"aarch64", "x86_64"})
    for (unsigned Case : {0u, 1u}) {
      SCOPED_TRACE(Arch + ":" + std::to_string(Case));
      auto Image = neverd::loadBinary(compileFixture(Arch, Case).string());
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      const std::string Name = Case == 0 ? "finite_ro" : "finite_rw";
      const auto Low = build(*Image, Name);
      ASSERT_EQ(Low.JumpTables.size(), 1u);
      const auto &Table = Low.JumpTables.front();
      EXPECT_EQ(Table.CaseLabels, (std::vector<int64_t>{2, 3}));
      EXPECT_TRUE(Table.HasDispatchSlotMap);
      EXPECT_EQ(Table.SlotIndices, (std::vector<uint32_t>{2, 3}));
      const auto *Two = Image->findSymbol(Name + "_case2");
      const auto *Three = Image->findSymbol(Name + "_case3");
      ASSERT_NE(Two, nullptr);
      ASSERT_NE(Three, nullptr);
      EXPECT_EQ(Table.Targets,
                (std::vector<neverd::va_t>{Two->Addr, Three->Addr}));
      EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    }
}

TEST_F(JumpTableFiniteSelector, UnknownOrChangedSelectorsDoNotPublish) {
  const char *Names[] = {"finite_unknown_arm", "finite_bypassed",
                         "finite_clobbered"};
  for (const std::string Arch : {"aarch64", "x86_64"})
    for (unsigned I = 0; I < std::size(Names); ++I) {
      SCOPED_TRACE(Arch + ":" + Names[I]);
      auto Image = neverd::loadBinary(compileFixture(Arch, I + 2).string());
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      const auto Low = build(*Image, Names[I]);
      EXPECT_TRUE(Low.JumpTables.empty());
      EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
      EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    }
}

TEST_F(JumpTableFiniteSelector,
       WritableDestinationMutationCannotPublishSource) {
  // The module-wide writer audit runs after the candidate-local CFG builder.
  // Exercise the publication boundary, including both C routes.
  for (const std::string Arch : {"aarch64", "x86_64"}) {
    const fs::path Object = compileFixture(Arch, 5);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Arch + (LLVM ? ":llvm" : ":high"));
      const auto File = tmpFile("mutated.c");
      std::vector<std::string> Args = {"decompile", Object.string(),
                                       "--func",    "finite_mutated",
                                       "-o",        File.string()};
      if (LLVM)
        Args.push_back("--llvm");
      const auto Recovered = exec(ndBin(), Args);
      if (Recovered.ok()) {
        std::ifstream Input(File);
        const std::string Source((std::istreambuf_iterator<char>(Input)), {});
        EXPECT_NE(Source.find("__builtin_trap"), std::string::npos);
      }
    }
  }
}

TEST_F(JumpTableFiniteSelector, ReachedBackedgeCannotReuseTheEntryDomain) {
  auto Image = neverd::loadBinary(compileFixture("aarch64", 6).string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto Low = build(*Image, "finite_recurrent");
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
}

TEST_F(JumpTableFiniteSelector, ExhaustedEvidenceDoesNotPublish) {
  for (const std::string Arch : {"aarch64", "x86_64"}) {
    auto Image = neverd::loadBinary(compileFixture(Arch, 0).string());
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    for (size_t Budget : {size_t{0}, size_t{64}}) {
      SCOPED_TRACE(Arch + ":" + std::to_string(Budget));
      const auto Low = build(*Image, "finite_ro", Budget);
      EXPECT_TRUE(Low.JumpTables.empty());
      EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
      EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    }
  }
}

TEST_F(JumpTableFiniteSelector, BothSourceRoutesExecuteTheOriginalSelection) {
  for (const std::string Arch : {"aarch64", "x86_64"})
    for (unsigned Case : {0u, 1u}) {
      const fs::path Object = compileFixture(Arch, Case);
      const std::string Name = Case == 0 ? "finite_ro" : "finite_rw";
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(Arch + ":" + Name + ":" + std::to_string(Route));
        const auto File = tmpFile("selection.c");
        std::vector<std::string> Args = {
            "decompile", Object.string(), "--func", Name, "-o", File.string()};
        if (Route != 0)
          Args.push_back("--llvm");
        if (Route == 2)
          Args.push_back("--no-opt");
        const auto Recovered = exec(ndBin(), Args);
        ASSERT_TRUE(Recovered.ok()) << Recovered.err;
        std::ifstream Input(File);
        const std::string Source((std::istreambuf_iterator<char>(Input)), {});
        ASSERT_EQ(Source.find("unresolved indirect branch"), std::string::npos);
        std::ofstream Out(File, std::ios::app);
        Out << "\nint main(void) {\n"
               "const uint32_t inputs[] = {0, 1, 2, 99, UINT32_MAX, "
               "UINT32_C(0x80000000)};\n"
               "for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i)"
               "if ("
            << Name << "(inputs[i]) != (inputs[i] == 0 ? 99 : 41)) return 1;\n"
            << "return 0; }\n";
        Out.close();
        for (const char *Optimization : {"-O0", "-O2"}) {
          const auto Exe = tmpFile(std::string("selection") +
                                   neverd::test::executableSuffix());
          const auto Built = exec(
              NEVERD_TEST_CLANG,
              {"-std=c11", Optimization, "-fsanitize=undefined",
               "-fsanitize-trap=undefined", File.string(), "-o", Exe.string()});
          ASSERT_TRUE(Built.ok()) << Built.err;
          const auto Run = exec(Exe.string(), {});
          EXPECT_TRUE(Run.ok()) << Run.err;
        }
      }
    }
}

} // namespace
