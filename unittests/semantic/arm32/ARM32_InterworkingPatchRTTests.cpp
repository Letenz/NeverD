//===- ARM32_InterworkingPatchRTTests.cpp - mixed-state ELF execution -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lift/NeverDLiftFixture.h"

#include "neverd/loader/ELF/ELFLoader.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unicorn/arm.h>
#include <unicorn/unicorn.h>
#include <utility>

namespace {

using namespace neverd;

class ARM32InterworkingPatchRT : public NeverDLiftTest {
protected:
  void checkExecutable(const fs::path &Path) {
    auto Image = ELFLoader().load(Path);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());

    std::map<std::string, uint32_t> Entries;
    for (const Symbol *Symbol : Image->getFunctionSymbols())
      if (Symbol->Name == "arm_leaf" || Symbol->Name == "arm_call_thumb" ||
          Symbol->Name == "thumb_leaf" || Symbol->Name == "thumb_call_arm")
        Entries[Symbol->Name] = static_cast<uint32_t>(Symbol->Addr);
    ASSERT_EQ(Entries.size(), 4u);

    for (const auto &[Name, Entry] : Entries) {
      const bool Thumb = Name.starts_with("thumb_");
      for (const auto &[Left, Right] :
           {std::pair<uint32_t, uint32_t>{7, 5}, {0xffffffffu, 2u}}) {
        SCOPED_TRACE(Path.string() + ":" + Name);
        uc_engine *Engine = nullptr;
        ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &Engine), UC_ERR_OK);
        std::set<uint64_t> Pages;
        for (const Segment &Segment : Image->Segments)
          for (uint64_t Page = Segment.VA & ~uint64_t{0xfff};
               Page < Segment.VA + Segment.Size; Page += 0x1000)
            if (Pages.insert(Page).second)
              ASSERT_EQ(uc_mem_map(Engine, Page, 0x1000, UC_PROT_ALL),
                        UC_ERR_OK);
        ASSERT_EQ(uc_mem_map(Engine, 0x300000, 0x10000, UC_PROT_ALL),
                  UC_ERR_OK);
        ASSERT_EQ(uc_mem_map(Engine, 0x400000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
        for (const Segment &Segment : Image->Segments)
          if (!Segment.Data.empty())
            ASSERT_EQ(uc_mem_write(Engine, Segment.VA, Segment.Data.data(),
                                   Segment.Data.size()),
                      UC_ERR_OK);

        uint32_t SP = 0x308000;
        uint32_t LR = 0x400000;
        uint32_t Arg0 = Left;
        uint32_t Arg1 = Right;
        ASSERT_EQ(uc_reg_write(Engine, UC_ARM_REG_R0, &Arg0), UC_ERR_OK);
        ASSERT_EQ(uc_reg_write(Engine, UC_ARM_REG_R1, &Arg1), UC_ERR_OK);
        ASSERT_EQ(uc_reg_write(Engine, UC_ARM_REG_SP, &SP), UC_ERR_OK);
        ASSERT_EQ(uc_reg_write(Engine, UC_ARM_REG_LR, &LR), UC_ERR_OK);
        const uc_err Error =
            uc_emu_start(Engine, Entry | (Thumb ? 1u : 0u), LR, 0, 1000);
        EXPECT_EQ(Error, UC_ERR_OK) << uc_strerror(Error);
        uint32_t Result = 0;
        uint32_t PC = 0;
        ASSERT_EQ(uc_reg_read(Engine, UC_ARM_REG_R0, &Result), UC_ERR_OK);
        ASSERT_EQ(uc_reg_read(Engine, UC_ARM_REG_PC, &PC), UC_ERR_OK);
        EXPECT_EQ(Result, static_cast<uint32_t>(Left + Right));
        EXPECT_EQ(PC, LR);
        ASSERT_EQ(uc_close(Engine), UC_ERR_OK);
      }
    }
  }
};

TEST_F(ARM32InterworkingPatchRT, SectionAndInplacePreserveCrossModeCalls) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target clang is unavailable";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  const fs::path Assembly = tmpFile("interworking.s");
  std::ofstream(Assembly) << R"(
.syntax unified
.text
.arm
.p2align 2
.globl arm_leaf
.type arm_leaf,%function
arm_leaf:
  add r0, r0, r1
  bx lr
.size arm_leaf, .-arm_leaf
.globl arm_call_thumb
.type arm_call_thumb,%function
arm_call_thumb:
  push {lr}
  blx thumb_leaf
  pop {pc}
.size arm_call_thumb, .-arm_call_thumb
.thumb
.p2align 1
.globl thumb_leaf
.type thumb_leaf,%function
.thumb_func
thumb_leaf:
  adds r0, r0, r1
  bx lr
.size thumb_leaf, .-thumb_leaf
.globl thumb_call_arm
.type thumb_call_arm,%function
.thumb_func
thumb_call_arm:
  push {lr}
  cbz r0, thumb_call_arm_direct
  adds r0, r0, #0
thumb_call_arm_direct:
  blx arm_leaf
  pop {pc}
.size thumb_call_arm, .-thumb_call_arm
)";
  const fs::path Object = tmpFile("interworking.o");
  const fs::path Original = tmpFile("interworking.elf");
  const auto Assembled =
      exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                               Assembly.string(), "-o", Object.string()});
  ASSERT_TRUE(Assembled.ok()) << Assembled.err;
  const auto Linked =
      exec("ld.lld", {"-m", "armelf_linux_eabi", "-e", "arm_leaf",
                      Object.string(), "-o", Original.string()});
  ASSERT_TRUE(Linked.ok()) << Linked.err;
  checkExecutable(Original);

  for (const auto &[Mode, Option] :
       {std::pair<const char *, const char *>{"section", "--mode=section"},
        {"inplace", "--mode=inplace"}}) {
    const fs::path Patched =
        tmpFile(std::string("interworking-") + Mode + ".elf");
    const auto Result = exec(
        ndBin(), {"patch", Option, "-o", Patched.string(), Original.string()});
    ASSERT_TRUE(Result.ok()) << Result.err;
    checkExecutable(Patched);
    const auto Lifted = exec(ndBin(), {"lift", "--dump-low", Patched.string()});
    ASSERT_TRUE(Lifted.ok()) << Lifted.err;
    for (const char *Name : {"arm_call_thumb", "thumb_call_arm"}) {
      const size_t Function =
          Lifted.out.find(std::string("func ") + Name + " @");
      ASSERT_NE(Function, std::string::npos) << Lifted.out;
      const size_t NextFunction = Lifted.out.find("\nfunc ", Function + 1);
      const std::string Body =
          Lifted.out.substr(Function, NextFunction - Function);
      EXPECT_TRUE(Body.find("BRANCH  cst:") != std::string::npos ||
                  Body.find("CALL reg:") != std::string::npos)
          << Body;
      EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
    }
  }
}

} // namespace
