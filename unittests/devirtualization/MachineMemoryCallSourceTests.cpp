//===- MachineMemoryCallSourceTests.cpp - Memory-call source execution ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace {
using namespace neverd;

std::string memoryCallFile(const fs::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

va_t memoryCallSymbol(const BinaryImage &Image, llvm::StringRef Name) {
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.Name == Name)
      return Symbol.Addr;
  return InvalidVA;
}

class MachineMemoryCallSourceTest : public NeverDLiftTest {
protected:
  static fs::path fixture(const char *Name) {
    return fs::path(TEST_SOURCE_DIR) / "fixtures" / Name;
  }

  RunResult buildMachine(const fs::path &Output) {
    return exec(NEVERD_TEST_CLANG,
                {"-target", "x86_64-linux-gnu", "-fuse-ld=lld", "-nostdlib",
                 "-static", "-Wl,-e,generic_memory_rip_call",
                 fixture("generic_machine_memory_calls.S").string(),
                 fixture("generic_machine_control_observer.S").string(), "-o",
                 Output.string()});
  }

  RunResult recover(const fs::path &Binary, llvm::StringRef Name, bool LLVM,
                    const fs::path &Source, const fs::path &Report,
                    bool MachineState = true) {
    std::vector<std::string> Args{
        "decompile", Binary.string(),  "--func",
        Name.str(),  "--devirtualize", "--recovery-report=" + Report.string(),
        "-o",        Source.string()};
    if (MachineState)
      Args.push_back("--vm-machine-state");
    if (LLVM)
      Args.push_back("--llvm");
    return exec(ndBin(), Args);
  }
};

struct MemoryCallCase {
  const char *Name;
  unsigned Kind;
  bool LLVM;
};

class MemoryCallRoundTripTest
    : public MachineMemoryCallSourceTest,
      public ::testing::WithParamInterface<MemoryCallCase> {};

TEST_P(MemoryCallRoundTripTest,
       CapturesMemoryTargetBeforeExactNativeStackEffects) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public memory-call source checks require clang";
  const auto &[Name, Kind, LLVM] = GetParam();
  const auto Binary = tmpFile("memory-calls.elf");
  const auto Built = buildMachine(Binary);
  ASSERT_TRUE(Built.ok()) << Built.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Source = tmpFile("recovered.c");
  const auto Report = tmpFile("recovery.json");
  const auto Recovered = recover(Binary, Name, LLVM, Source, Report);
  ASSERT_TRUE(Recovered.ok()) << Recovered.err << memoryCallFile(Report);
  auto JSON = llvm::json::parse(memoryCallFile(Report));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(Object->getBoolean("complete"), true);
  EXPECT_EQ(Object->getBoolean("controlComplete"), true);
  EXPECT_EQ(Object->getBoolean("discoverControlState"), true);
  for (const char *Key : {"controlRegisters", "controlFrameSlots"}) {
    const auto *Fields = Object->getArray(Key);
    ASSERT_NE(Fields, nullptr);
    EXPECT_TRUE(Fields->empty())
        << "No hand-picked control fields are supplied";
  }
  if (Kind < 2) {
    const auto *Reads = Object->getArray("immutableReads");
    ASSERT_NE(Reads, nullptr);
    EXPECT_FALSE(Reads->empty());
  }

  // The oracle observes code addresses as data. Follow ELF symbols rather
  // than assuming the linker's current section placement or instruction size.
  std::string Addresses;
  for (const char *Symbol :
       {"generic_memory_callee_zero", "generic_memory_callee_one",
        "generic_memory_rip_continuation", "generic_memory_table_continuation",
        "generic_memory_stack_continuation",
        "generic_memory_overwritten_continuation",
        "generic_memory_readonly_table", "generic_machine_observer_exit"}) {
    const va_t Address = memoryCallSymbol(*Image, Symbol);
    ASSERT_NE(Address, InvalidVA) << Symbol;
    Addresses += "#define MEMORY_ADDRESS_" + std::string(Symbol) +
                 " UINT64_C(" + std::to_string(Address) + ")\n";
  }
  const auto Harness = tmpFile("reference.c");
  std::ofstream(Harness) << memoryCallFile(Source)
                         << "\n#define GENERIC_MEMORY_SOURCE\n"
                         << "#define GENERIC_MEMORY_FUNCTION " << Name << "\n"
                         << "#define GENERIC_MEMORY_KIND " << Kind << "\n"
                         << Addresses
                         << "#define MEMORY_ADDRESS(s) MEMORY_ADDRESS_##s\n"
                         << memoryCallFile(
                                fixture("generic_machine_memory_reference.c"));
  std::ofstream(tmpFile("immintrin.h")).close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Program = tmpFile(std::string("recovered-memory-call") +
                                 neverd::test::executableSuffix());
    const auto Compiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-fno-inline", "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err << memoryCallFile(Source);
    const auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << " exit=" << Ran.exitCode << '\n'
                          << memoryCallFile(Source);
  }
}

INSTANTIATE_TEST_SUITE_P(
    PublicMemoryCalls, MemoryCallRoundTripTest,
    ::testing::Values(
        MemoryCallCase{"generic_memory_rip_call", 0, false},
        MemoryCallCase{"generic_memory_rip_call", 0, true},
        MemoryCallCase{"generic_memory_table_call", 1, false},
        MemoryCallCase{"generic_memory_table_call", 1, true},
        MemoryCallCase{"generic_memory_stack_call", 2, false},
        MemoryCallCase{"generic_memory_stack_call", 2, true},
        MemoryCallCase{"generic_memory_overwritten_slot_call", 3, false},
        MemoryCallCase{"generic_memory_overwritten_slot_call", 3, true}),
    [](const ::testing::TestParamInfo<MemoryCallCase> &Info) {
      return std::string(Info.param.Name) +
             (Info.param.LLVM ? "_LLVMC" : "_HighC");
    });

TEST_F(MachineMemoryCallSourceTest,
       NativeMemoryCallsMatchFullIndependentState) {
#if !defined(__x86_64__) || !defined(__linux__)
  GTEST_SKIP() << "native fixture execution requires an x64 Linux host";
#endif
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "native public memory-call checks require clang";
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Program = tmpFile(std::string("native-memory-calls") +
                                 neverd::test::executableSuffix());
    const auto Compiled = exec(
        NEVERD_TEST_CLANG,
        {"-target", "x86_64-linux-gnu", "-fuse-ld=lld", "-std=c11", "-no-pie",
         Optimization, fixture("generic_machine_memory_calls.S").string(),
         fixture("generic_machine_control_observer.S").string(),
         fixture("generic_machine_memory_reference.c").string(), "-o",
         Program.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err;
    const auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << " exit=" << Ran.exitCode;
  }
}

TEST_F(MachineMemoryCallSourceTest, UnknownOrInvalidMemoryTargetsNeverPublish) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public memory-call refusal checks require clang";
  const auto Binary = tmpFile("memory-calls.elf");
  const auto Built = buildMachine(Binary);
  ASSERT_TRUE(Built.ok()) << Built.err;
  for (const char *Name :
       {"generic_memory_external_call", "generic_memory_writable_call",
        "generic_memory_unknown_stack_call",
        "generic_memory_alias_clobbered_call",
        "generic_memory_missing_target_call",
        "generic_memory_nonexec_target_call"})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      const std::string Stem = std::string(Name) + std::to_string(LLVM);
      const auto Source = tmpFile(Stem + ".c");
      const auto Report = tmpFile(Stem + ".json");
      const auto Recovered = recover(Binary, Name, LLVM, Source, Report);
      EXPECT_FALSE(Recovered.ok());
      EXPECT_FALSE(fs::exists(Source));
      auto JSON = llvm::json::parse(memoryCallFile(Report));
      ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
      const auto *Object = JSON->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), false);
      EXPECT_EQ(Object->getBoolean("controlComplete"), false);
      const auto Status = Object->getString("status");
      EXPECT_TRUE(Status == "unsupported" || Status == "unresolved-control")
          << memoryCallFile(Report);
    }
}

TEST_F(MachineMemoryCallSourceTest, OrdinarySourceStillRefusesMemoryCalls) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public ordinary source contract checks require clang";
  const auto Binary = tmpFile("memory-calls.elf");
  const auto Built = buildMachine(Binary);
  ASSERT_TRUE(Built.ok()) << Built.err;
  for (const char *Name :
       {"generic_memory_rip_call", "generic_memory_table_call",
        "generic_memory_stack_call", "generic_memory_overwritten_slot_call"})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Name);
      const std::string Stem = std::string(Name) + std::to_string(LLVM);
      const auto Source = tmpFile(Stem + ".c");
      const auto Report = tmpFile(Stem + ".json");
      EXPECT_FALSE(recover(Binary, Name, LLVM, Source, Report, false).ok());
      EXPECT_FALSE(fs::exists(Source));
      auto JSON = llvm::json::parse(memoryCallFile(Report));
      ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
      const auto *Object = JSON->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), false);
      EXPECT_EQ(Object->getBoolean("controlComplete"), false);
    }
}

TEST_F(MachineMemoryCallSourceTest,
       MachineSourceUsesOriginalWritableMappingsThroughBothRoutes) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "fixed guest mappings require an x64 Linux host";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  const auto Assembly = tmpFile("fixed-memory.S");
  std::ofstream(Assembly) << R"(
.text
.globl generic_fixed_memory
.type generic_fixed_memory,@function
generic_fixed_memory:
  movq generic_fixed_words(%rip), %rax
  leaq 17(%rax), %rax
  leaq generic_fixed_words(%rip), %rcx
  movq %rax, generic_fixed_words+8(%rip)
  retq
.size generic_fixed_memory, .-generic_fixed_memory
.data
.p2align 3
.globl generic_fixed_words
generic_fixed_words:
  .quad 31, 0, 0
.section .note.GNU-stack,"",@progbits
)";
  const auto Binary = tmpFile("fixed-memory.elf");
  const auto Built = exec(
      NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-fuse-ld=lld",
                          "-nostdlib", "-static", "-Wl,-e,generic_fixed_memory",
                          Assembly.string(), "-o", Binary.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const va_t Address = memoryCallSymbol(*Image, "generic_fixed_words");
  ASSERT_NE(Address, InvalidVA);
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
    const auto Source = tmpFile(LLVM ? "fixed-llvm.c" : "fixed-high.c");
    const auto Report = tmpFile(LLVM ? "fixed-llvm.json" : "fixed-high.json");
    const auto Recovered =
        recover(Binary, "generic_fixed_memory", LLVM, Source, Report);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err << memoryCallFile(Report);
    const auto Harness = tmpFile("fixed-reference.c");
    std::ofstream(Harness) << "#define _GNU_SOURCE\n"
                           << memoryCallFile(Source)
                           << "\n#define GUEST_ADDRESS UINT64_C(" << Address
                           << ")\n"
                           << R"(
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return 10;
  uintptr_t base = GUEST_ADDRESS - GUEST_ADDRESS % (uintptr_t)page_size;
  void *requested = (void *)base;
  size_t length = (size_t)(GUEST_ADDRESS - base) + 3 * sizeof(uint64_t);
  void *mapping = mmap(requested, length, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) return 11;
  if (mapping != requested) { munmap(mapping, length); return 12; }
  uint64_t *memory = (uint64_t *)(uintptr_t)GUEST_ADDRESS;
  const uint64_t inputs[] = {0, 1, UINT64_MAX - 16, UINT64_MAX};
  for (unsigned sample = 0; sample < 4; ++sample) {
    uint64_t state[17], before[17], stack[8];
    for (unsigned i = 0; i < 16; ++i) state[i] = 100 + i;
    state[4] = (uint64_t)(uintptr_t)&stack[4];
    state[16] = 0x202;
    for (unsigned i = 0; i < 17; ++i) before[i] = state[i];
    memory[0] = inputs[sample];
    memory[1] = 0;
    memory[2] = UINT64_C(0x4a5b6c7d8e9f1023);
    if (generic_fixed_memory((void *)state) != 0) return 1;
    if (state[0] != inputs[sample] + 17 || state[1] != GUEST_ADDRESS ||
        memory[0] != inputs[sample] || memory[1] != inputs[sample] + 17 ||
        memory[2] != UINT64_C(0x4a5b6c7d8e9f1023)) return 2;
    for (unsigned i = 2; i < 17; ++i)
      if (state[i] != before[i]) return 3;
  }
  return munmap(mapping, length) != 0;
}
)";
    std::ofstream(tmpFile("immintrin.h")).close();
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Program = tmpFile(std::string("fixed-memory-source") +
                                   neverd::test::executableSuffix());
      const auto Compiled =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", Optimization, "-Werror=return-type",
                "-Werror=implicit-function-declaration", "-fsanitize=undefined",
                "-fsanitize-trap=undefined", "-I", tmp().string(),
                Harness.string(), "-o", Program.string()});
      ASSERT_TRUE(Compiled.ok()) << Compiled.err << memoryCallFile(Source);
      const auto Ran = exec(Program.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err << " exit=" << Ran.exitCode << '\n'
                            << memoryCallFile(Source);
    }
  }
#endif
}

} // namespace
