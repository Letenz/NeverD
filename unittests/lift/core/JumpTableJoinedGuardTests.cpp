//===- JumpTableJoinedGuardTests.cpp - Joined range guards ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks entry and backedge guards through native decoding and both C routes.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryLoading.h"

namespace {

class JumpTableJoinedGuard : public NeverDLiftTest {
protected:
  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  fs::path fixture(const std::string &Format, unsigned Variant) {
    const bool COFF = Format == "coff", MachO = Format == "macho";
    const fs::path Source = tmpFile(Format + std::to_string(Variant) + ".s");
    const fs::path Object = tmpFile(Format + std::to_string(Variant) + ".o");
    std::ofstream Out(Source);
    Out << (MachO ? ".section __TEXT,__text,regular,pure_instructions\n"
                  : ".text\n")
        << ".globl jt_joined_guard\n";
    if (COFF)
      Out << ".def jt_joined_guard; .scl 2; .type 32; .endef\n";
    else if (!MachO)
      Out << ".type jt_joined_guard,@function\n";
    Out << "jt_joined_guard:\n  lea .Ltable(%rip),%r8\n  mov "
        << (COFF ? (Variant == 5 ? "%rcx" : "%ecx")
                 : (Variant == 5 ? "%rdi" : "%edi"))
        << (Variant == 5 ? ",%r10\n" : ",%r10d\n");
    auto Guard = [&] {
      Out << (Variant == 7 ? "  cmp $3,%r10d\n  jae .Lexit\n"
                           : "  cmp $2,%r10d\n  ja .Lexit\n");
    };
    if (Variant != 1)
      Guard();
    Out << ".Ldispatch:\n";
    if (Variant == 1)
      Guard();
    if (Variant == 3)
      Out << "  mov $255,%r10b\n";
    Out << "  movslq (%r8,%r10,4),%rax\n"
           "  add %r8,%rax\n  jmp *%rax\n.Lzero:\n  mov "
        << (COFF ? (Variant == 5 ? "%rdx" : "%edx")
                 : (Variant == 5 ? "%rsi" : "%esi"))
        << (Variant == 5 ? ",%r10\n" : ",%r10d\n");
    if (Variant == 1 || Variant == 2)
      Out << "  jmp .Ldispatch\n";
    else {
      Out << "  cmp $"
          << (Variant == 6   ? 5
              : Variant == 7 ? 3
                             : 2)
          << ",%r10d\n";
      // INC preserves CF but changes ZF. Those flags no longer describe one
      // index comparison; the equality arm can admit an out-of-range value.
      if (Variant == 4)
        Out << "  inc %r9d\n";
      Out << (Variant == 7 ? "  jb .Ldispatch\n" : "  jbe .Ldispatch\n");
    }
    Out << ".Lexit:\n  mov $99,%eax\n  ret\n"
           ".Lone:\n  mov $1,%eax\n  ret\n"
           ".Ltwo:\n  mov $2,%eax\n  ret\n";
    if (!COFF && !MachO)
      Out << ".size jt_joined_guard,.-jt_joined_guard\n";
    Out << (MachO  ? ".section __TEXT,__const\n"
            : COFF ? ".section .rdata,\"dr\"\n"
                   : ".section .rodata\n")
        << ".p2align 2\n.Ltable:\n"
           "  .long .Lzero-.Ltable\n  .long .Lone-.Ltable\n"
           "  .long .Ltwo-.Ltable\n  .long .Lexit-.Ltable\n";
    if (!COFF && !MachO)
      Out << ".section .note.GNU-stack,\"\",@progbits\n";
    Out.close();
    const std::string Triple = COFF    ? "x86_64-pc-windows-msvc"
                               : MachO ? "x86_64-apple-darwin"
                                       : "x86_64-unknown-linux-gnu";
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-target", Triple, "-c", Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  static neverd::LowFunc build(const neverd::BinaryImage &Image,
                               neverd::CFGBuilder &Builder) {
    const auto *Function = Image.findSymbol("jt_joined_guard");
    EXPECT_NE(Function, nullptr);
    if (!Function)
      return {};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  }
};

TEST_F(JumpTableJoinedGuard, SeparateGuardsRecoverExactlyTheDenseCases) {
  for (const std::string Format : {"elf", "coff", "macho"}) {
    for (unsigned Variant : {0u, 1u, 7u}) {
      SCOPED_TRACE(Format + ":" + std::to_string(Variant));
      auto Image = neverd::loadBinary(fixture(Format, Variant));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      neverd::CFGBuilder Builder;
      const auto Low = build(*Image, Builder);
      ASSERT_EQ(Low.JumpTables.size(), 1u);
      EXPECT_EQ(Low.JumpTables.front().SlotIndices,
                (std::vector<uint32_t>{0, 1, 2}));
      EXPECT_EQ(Low.JumpTables.front().Targets.size(), 3u);
      EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
    }
  }
}

TEST_F(JumpTableJoinedGuard, UncheckedAndChangedInputsRemainUnresolved) {
  for (const std::string Format : {"elf", "coff", "macho"}) {
    for (unsigned Variant : {2u, 3u, 4u, 5u, 6u}) {
      SCOPED_TRACE(Format + ":" + std::to_string(Variant));
      auto Image = neverd::loadBinary(fixture(Format, Variant));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      neverd::CFGBuilder Builder;
      const auto Low = build(*Image, Builder);
      EXPECT_TRUE(Low.JumpTables.empty());
    }
  }
}

TEST_F(JumpTableJoinedGuard, ExhaustedEvidenceCannotKeepProvisionalTargets) {
  auto Image = neverd::loadBinary(fixture("elf", 0));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (bool SymbolBudget : {false, true}) {
    SCOPED_TRACE(SymbolBudget);
    neverd::CFGBuilder Builder;
    if (SymbolBudget)
      Builder.setFiniteSetSymbolEvidenceBudgetForTesting(0);
    else
      Builder.setMaskFixedPointEvidenceBudgetForTesting(0);
    const auto Low = build(*Image, Builder);
    EXPECT_TRUE(Low.JumpTables.empty());
    EXPECT_FALSE(Low.UnsafeIndirectBranchAddresses.empty());
  }
}

TEST_F(JumpTableJoinedGuard, GeneratedSourceExecutesEntryAndLoopSelections) {
  for (unsigned Variant : {0u, 7u}) {
    const fs::path Object = fixture("elf", Variant);
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(std::to_string(Variant) + ":" + std::to_string(Route));
      const fs::path Source = tmpFile("joined-" + std::to_string(Variant) +
                                      std::to_string(Route) + ".c");
      std::vector<std::string> Args = {"decompile", Object.string(),
                                       "--func",    "jt_joined_guard",
                                       "-o",        Source.string()};
      if (Route)
        Args.push_back("--llvm");
      if (Route == 2)
        Args.push_back("--no-opt");
      const auto Decompiled = exec(ndBin(), Args);
      ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
      std::ofstream Out(Source, std::ios::app);
      Out << R"(
static int check(uint64_t first, uint64_t next) {
  uint32_t index = (uint32_t)first;
  if (index == 0) index = (uint32_t)next;
  if (index == 0) return 0; /* The original loop does not terminate. */
  uint64_t expected = index <= 2 ? index : 99;
  return jt_joined_guard(first, next) != expected;
}
int main(void) {
  for (uint64_t a = 0; a < 256; ++a)
    for (uint64_t b = 0; b < 256; ++b)
      if (check(a, b)) return 1;
  const uint64_t edges[] = {0, 1, 2, 3, UINT32_MAX,
                            UINT64_C(0x100000000), UINT64_MAX};
  for (unsigned a = 0; a < sizeof(edges)/sizeof(edges[0]); ++a)
    for (unsigned b = 0; b < sizeof(edges)/sizeof(edges[0]); ++b)
      if (check(edges[a], edges[b])) return 2;
  return 0;
}
)";
      Out.close();
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Exe = tmpFile("joined-run");
        const auto Built = exec(
            NEVERD_TEST_CLANG,
            {"-std=c11", Optimization, "-fsanitize=undefined",
             "-fsanitize-trap=undefined", Source.string(), "-o", Exe.string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
        const auto Run = exec(Exe.string(), {});
        EXPECT_TRUE(Run.ok()) << Run.err;
      }
    }
  }
}

} // namespace
