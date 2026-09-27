//===- ELFARM32ModeTests.cpp - ELF ARM instruction-mode evidence ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/loader/ELF/ELFLoader.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"

#include <cstring>

namespace {

using namespace neverd;

class ELFARM32ModeTest : public NeverDLiftTest {
protected:
  llvm::Expected<BinaryImage> loadAssembly(const std::string &Source,
                                           uint32_t StrippedEntry = 0) {
    const auto Assembly = tmpFile("mode.s");
    std::ofstream(Assembly) << Source;
    const auto Object = tmpFile("mode.o");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                                 Assembly.string(), "-o", Object.string()});
    if (!Compiled.ok())
      return llvm::make_error<llvm::StringError>(
          Compiled.err, llvm::inconvertibleErrorCode());
    if (StrippedEntry != 0) {
      // Model a linked, stripped image: retain the actual instruction bytes,
      // assign the allocated code its linked VA, and remove symbol evidence.
      using namespace llvm::ELF;
      using ELFT = llvm::object::ELF32LE;
      std::ifstream Input(Object, std::ios::binary);
      std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
      Input.close();
      ELFT::Ehdr Header;
      std::memcpy(&Header, Bytes.data(), sizeof(Header));
      Header.e_type = ET_EXEC;
      Header.e_entry = StrippedEntry;
      std::memcpy(Bytes.data(), &Header, sizeof(Header));
      for (unsigned Index = 0; Index != Header.e_shnum; ++Index) {
        const auto Offset = Header.e_shoff + Index * Header.e_shentsize;
        ELFT::Shdr Section;
        std::memcpy(&Section, Bytes.data() + Offset, sizeof(Section));
        if (Section.sh_flags & SHF_ALLOC)
          Section.sh_addr = StrippedEntry & ~1u;
        if (Section.sh_type == SHT_SYMTAB || Section.sh_type == SHT_DYNSYM)
          Section.sh_type = SHT_NULL;
        std::memcpy(Bytes.data() + Offset, &Section, sizeof(Section));
      }
      std::ofstream Output(Object, std::ios::binary | std::ios::trunc);
      Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    }
    return ELFLoader().load(Object);
  }
};

TEST_F(ELFARM32ModeTest, PreservesThumbModeBeforeNormalizingFunctionAddresses) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.thumb
.globl add32
.type add32,%function
.thumb_func
add32:
  adds r0, r0, r1
  bx lr
.size add32, .-add32
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::Thumb);
  const auto Symbols = Image->getFunctionSymbols();
  ASSERT_EQ(Symbols.size(), 1u);
  EXPECT_EQ(Symbols.front()->Addr, 0u);
  EXPECT_EQ(Symbols.front()->Size, 4u);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image->Arch, Image->Mode));
  const auto *Segment = Image->getSegmentFor(Symbols.front()->Addr);
  ASSERT_NE(Segment, nullptr);
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOne(Segment->Data.data(), Segment->Data.size(), 0, Insn),
            2);
  EXPECT_STREQ(Insn.Raw->mnemonic, "adds");
}

TEST_F(ELFARM32ModeTest, ARMCodeDoesNotInheritModeFromOddDataOrArbitraryNames) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.globl add32
.type add32,%function
add32:
  add r0, r0, r1
  bx lr
.size add32, .-add32
.local $thing
$thing:
  nop
.data
.byte 0
.type odd_data,%object
odd_data:
  .byte 1
.local $t.data
$t.data:
  .byte 2
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::ARM);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image->Arch, Image->Mode));
  const auto *Segment = Image->getSegmentFor(0);
  ASSERT_NE(Segment, nullptr);
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOne(Segment->Data.data(), Segment->Data.size(), 0, Insn),
            4);
  EXPECT_STREQ(Insn.Raw->mnemonic, "add");
}

TEST_F(ELFARM32ModeTest, MappingSymbolRetainsThumbModeWithoutAFunctionSymbol) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.thumb
  adds r0, r0, r1
  bx lr
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::Thumb);
  EXPECT_TRUE(Image->getFunctionSymbols().empty());
}

TEST_F(ELFARM32ModeTest, EmbeddedDataHasNoInstructionMode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.globl arm_before
.type arm_before,%function
arm_before:
  add r0, r0, r1
  bx lr
.size arm_before, .-arm_before
.word 0xe0800001
.thumb
.globl thumb_after
.type thumb_after,%function
.thumb_func
thumb_after:
  adds r0, r0, r1
  bx lr
.size thumb_after, .-thumb_after
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->instructionModeAt(0), InstructionMode::ARM);
  EXPECT_FALSE(Image->instructionModeAt(8));
  EXPECT_EQ(Image->instructionModeAt(12), InstructionMode::Thumb);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  EXPECT_FALSE(Dec.selectMode(*Image, 8, InstructionMode::ARM));
  EXPECT_TRUE(Dec.selectMode(*Image, 12));
  EXPECT_EQ(Dec.currentMode(), InstructionMode::Thumb);
}

TEST_F(ELFARM32ModeTest, LinkedEntryRetainsModeWithoutSymbolEvidence) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  for (bool Thumb : {false, true}) {
    SCOPED_TRACE(Thumb);
    auto Image = loadAssembly(std::string(".syntax unified\n.text\n") +
                                  (Thumb ? ".thumb\nadds r0, r0, r1\nbx lr\n"
                                         : ".arm\nadd r0, r0, r1\nbx lr\n"),
                              Thumb ? 0x1001 : 0x1000);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Mode,
              Thumb ? InstructionMode::Thumb : InstructionMode::ARM);
    EXPECT_EQ(Image->Entry, 0x1000u);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image->Arch, Image->Mode));
    const auto *Segment = Image->getSegmentFor(Image->Entry);
    ASSERT_NE(Segment, nullptr);
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOne(Segment->Data.data(), Segment->Data.size(),
                            Image->Entry, Insn),
              Thumb ? 2 : 4);
    EXPECT_STREQ(Insn.Raw->mnemonic, Thumb ? "adds" : "add");
  }
}

TEST_F(ELFARM32ModeTest, DirectModeEvidenceOverridesWeakImageFallback) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
  blx thumb_target
  bx lr
.thumb
.thumb_func
thumb_target:
  adds r0, r0, r1
  bx lr
)",
                            0x1000);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Mode, InstructionMode::ARM);
  EXPECT_EQ(Image->instructionModeAt(0x1000), InstructionMode::ARM);
  EXPECT_FALSE(Image->instructionModeAt(0x1000, InstructionMode::Thumb));
  EXPECT_EQ(Image->instructionModeAt(0x1008), InstructionMode::ARM);
  EXPECT_EQ(Image->instructionModeAt(0x1008, InstructionMode::Thumb),
            InstructionMode::Thumb);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  ASSERT_TRUE(Dec.selectMode(*Image, 0x1008, InstructionMode::Thumb));
  EXPECT_EQ(Dec.currentMode(), InstructionMode::Thumb);
}

TEST_F(ELFARM32ModeTest, UsesAddressSpecificModesInMixedImages) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  const std::string ARM = R"(
.arm
.p2align 2
.globl arm32_add
.type arm32_add,%function
arm32_add:
  add r0, r0, r1
  bx lr
.size arm32_add, .-arm32_add
)";
  const std::string Thumb = R"(
.thumb
.p2align 1
.globl thumb32_add
.type thumb32_add,%function
.thumb_func
thumb32_add:
  adds r0, r0, r1
  bx lr
.size thumb32_add, .-thumb32_add
)";
  for (bool ThumbFirst : {false, true}) {
    SCOPED_TRACE(ThumbFirst);
    auto Image = loadAssembly(".syntax unified\n.text\n" +
                              (ThumbFirst ? Thumb + ARM : ARM + Thumb));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Mode, InstructionMode::MixedARMThumb);
    EXPECT_EQ(Image->getFunctionSymbols().size(), 2u);
    const auto Functions = Image->getFunctionSymbols();
    ASSERT_EQ(Functions.size(), 2u);
    for (const Symbol *Function : Functions)
      EXPECT_EQ(Image->instructionModeAt(Function->Addr),
                Function->Name == "thumb32_add" ? InstructionMode::Thumb
                                                : InstructionMode::ARM);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(*Image));
    for (const Symbol *Function : Functions) {
      ASSERT_TRUE(Dec.selectMode(*Image, Function->Addr));
      EXPECT_EQ(Dec.currentMode(), Image->instructionModeAt(Function->Addr));
    }
    const auto Object = tmpFile("mode.o").string();
    const auto Headers = exec(ndBin(), {"headers", "--json", Object});
    ASSERT_TRUE(Headers.ok()) << Headers.err;
    EXPECT_NE(Headers.out.find("mixed_arm_thumb"), std::string::npos)
        << Headers.out;
    const auto Symbols = exec(ndBin(), {"symbols", "--json", Object});
    ASSERT_TRUE(Symbols.ok()) << Symbols.err;
    EXPECT_NE(Symbols.out.find("arm32_add"), std::string::npos);
    EXPECT_NE(Symbols.out.find("thumb32_add"), std::string::npos);
    const auto Lifted = exec(ndBin(), {"lift", "--dump-low", Object});
    EXPECT_TRUE(Lifted.ok()) << Lifted.err;
    EXPECT_NE(Lifted.out.find("arm32_add"), std::string::npos);
    EXPECT_NE(Lifted.out.find("thumb32_add"), std::string::npos);
  }
}

TEST_F(ELFARM32ModeTest, RejectsAThumbFunctionAliasOverARMMappingEvidence) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.globl arm32_add
.type arm32_add,%function
arm32_add:
  add r0, r0, r1
  bx lr
.size arm32_add, .-arm32_add
.globl conflicting_alias
.type conflicting_alias,%function
.set conflicting_alias, arm32_add + 1
)");
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError())
                .find("conflicting ARM/Thumb instruction modes"),
            std::string::npos);
}

TEST_F(ELFARM32ModeTest, DecompilesCallsAcrossARMAndThumbRegions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
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
  blx arm_leaf
  pop {pc}
.size thumb_call_arm, .-thumb_call_arm
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Mode, InstructionMode::MixedARMThumb);
  ASSERT_EQ(Image->getFunctionSymbols().size(), 4u);
  const uint8_t *ARMCall = Image->readVA(12, 4);
  const uint8_t *ThumbCall = Image->readVA(26, 4);
  ASSERT_NE(ARMCall, nullptr);
  ASSERT_NE(ThumbCall, nullptr);
  uint32_t ARMEncoding;
  uint16_t ThumbEncoding[2];
  std::memcpy(&ARMEncoding, ARMCall, sizeof(ARMEncoding));
  std::memcpy(ThumbEncoding, ThumbCall, sizeof(ThumbEncoding));
  EXPECT_EQ(ARMEncoding, 0xfa000000u);
  EXPECT_EQ(ThumbEncoding[0], 0xf7ffu);
  EXPECT_EQ(ThumbEncoding[1], 0xeff2u);
  const auto Object = tmpFile("mode.o").string();
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
    const auto Output = tmpFile(LLVM ? "mixed-llvm.c" : "mixed-high.c");
    std::vector<std::string> Arguments{"decompile"};
    if (LLVM)
      Arguments.push_back("--llvm");
    Arguments.insert(Arguments.end(), {"-o", Output.string(), Object});
    const auto Decompiled = exec(ndBin(), Arguments);
    ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
    std::ifstream Input(Output);
    const std::string Source(std::istreambuf_iterator<char>(Input), {});
    EXPECT_NE(Source.find("arm_call_thumb"), std::string::npos) << Source;
    EXPECT_NE(Source.find("thumb_call_arm"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("return (int32_t)thumb_call_arm()"),
              std::string::npos)
        << Source;
    if (!LLVM) {
      EXPECT_NE(Source.find("arm_call_thumb(int32_t arg0, int32_t arg1)"),
                std::string::npos)
          << Source;
      EXPECT_NE(Source.find("thumb_call_arm(int32_t arg0, int32_t arg1)"),
                std::string::npos)
          << Source;
      EXPECT_NE(Source.find("thumb_leaf(arg0, arg1)"), std::string::npos)
          << Source;
      EXPECT_NE(Source.find("arm_leaf(arg0, arg1)"), std::string::npos)
          << Source;
    }
    EXPECT_EQ(Source.find("/* unknown"), std::string::npos) << Source;
    {
      std::ofstream CFile(Output, std::ios::app);
      CFile << "\nint main(void) {\n"
               "  return arm_call_thumb(7, 5) == 12 && "
               "thumb_call_arm(7, 5) == 12 && "
               "arm_call_thumb(0xffffffffu, 2) == 1 && "
               "thumb_call_arm(0xffffffffu, 2) == 1 ? 0 : 1;\n"
               "}\n";
    }
    const auto Executable = tmpFile(LLVM ? "mixed-llvm" : "mixed-high");
    const auto Compiled = exec(
        NEVERD_TEST_CLANG, {"-std=c11", "-Werror=implicit-function-declaration",
                            "-O0", Output.string(), "-o", Executable.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err;
  }
}

TEST_F(ELFARM32ModeTest, RecoversForwardedArgumentsAcrossModeChains) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.p2align 2
.globl arm_outer
.type arm_outer,%function
arm_outer:
  push {lr}
  blx thumb_middle
  pop {pc}
.size arm_outer, .-arm_outer
.thumb
.p2align 1
.globl thumb_middle
.type thumb_middle,%function
.thumb_func
thumb_middle:
  push {lr}
  blx arm_leaf
  pop {pc}
.size thumb_middle, .-thumb_middle
.arm
.p2align 2
.globl arm_leaf
.type arm_leaf,%function
arm_leaf:
  add r0, r0, r1
  bx lr
.size arm_leaf, .-arm_leaf
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto Output = tmpFile("chain.c");
  const auto Decompiled = exec(ndBin(), {"decompile", "-o", Output.string(),
                                         tmpFile("mode.o").string()});
  ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
  std::ifstream Input(Output);
  const std::string Source(std::istreambuf_iterator<char>(Input), {});
  EXPECT_NE(Source.find("arm_outer(int32_t arg0, int32_t arg1)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("thumb_middle(int32_t arg0, int32_t arg1)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("thumb_middle(arg0, arg1)"), std::string::npos)
      << Source;
  {
    std::ofstream CFile(Output, std::ios::app);
    CFile << "\nint main(void) { return arm_outer(7, 5) == 12 && "
             "arm_outer(0xffffffffu, 2) == 1 ? 0 : 1; }\n";
  }
  const auto Executable = tmpFile("chain");
  const auto Compiled = exec(
      NEVERD_TEST_CLANG, {"-std=c11", "-Werror=implicit-function-declaration",
                          "-O0", Output.string(), "-o", Executable.string()});
  ASSERT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
  EXPECT_TRUE(exec(Executable.string(), {}).ok());
}

TEST_F(ELFARM32ModeTest, AppliesThumbWideBranchRelocations) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM branch fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.thumb
.p2align 1
.globl thumb_branch
.type thumb_branch,%function
.thumb_func
thumb_branch:
  cmp r0, #0
  beq.w thumb_target
  b.w thumb_target
  bx lr
.size thumb_branch, .-thumb_branch
.globl thumb_target
.type thumb_target,%function
.thumb_func
thumb_target:
  adds r0, r0, #1
  bx lr
.size thumb_target, .-thumb_target
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const uint8_t *Conditional = Image->readVA(2, 4);
  const uint8_t *Unconditional = Image->readVA(6, 4);
  ASSERT_NE(Conditional, nullptr);
  ASSERT_NE(Unconditional, nullptr);
  uint16_t ConditionalHalfwords[2], UnconditionalHalfwords[2];
  std::memcpy(ConditionalHalfwords, Conditional, 4);
  std::memcpy(UnconditionalHalfwords, Unconditional, 4);
  EXPECT_EQ(ConditionalHalfwords[0], 0xf000u);
  EXPECT_EQ(ConditionalHalfwords[1], 0x8003u);
  EXPECT_EQ(UnconditionalHalfwords[0], 0xf000u);
  EXPECT_EQ(UnconditionalHalfwords[1], 0xb801u);
}

TEST_F(ELFARM32ModeTest, AppliesARMCallRelocationToHalfwordThumbEntry) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM branch fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.globl arm_to_thumb_half
.type arm_to_thumb_half,%function
arm_to_thumb_half:
  blx thumb_half
  bx lr
.size arm_to_thumb_half, .-arm_to_thumb_half
.thumb
  nop
.globl thumb_half
.type thumb_half,%function
.thumb_func
thumb_half:
  adds r0, r0, r1
  bx lr
.size thumb_half, .-thumb_half
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->instructionModeAt(10), InstructionMode::Thumb);
  const uint8_t *Call = Image->readVA(0, 4);
  ASSERT_NE(Call, nullptr);
  uint32_t Encoding = 0;
  std::memcpy(&Encoding, Call, sizeof(Encoding));
  EXPECT_EQ(Encoding, 0xfb000000u);
}

TEST_F(ELFARM32ModeTest, RetainsHalfwordBitInGeneratedARMToThumbCall) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  auto Source = loadAssembly(R"(
.syntax unified
.text
.arm
.globl arm_caller
.type arm_caller,%function
arm_caller:
  bx lr
.size arm_caller, .-arm_caller
.thumb
.globl thumb_callee
.type thumb_callee,%function
.thumb_func
thumb_callee:
  bx lr
.size thumb_callee, .-thumb_callee
)");
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_EQ(Source->Mode, InstructionMode::MixedARMThumb);

  CompiledImage Compiled;
  Compiled.Bytes.resize(14);
  const uint32_t PackedBL = 0xeb000000u;
  std::memcpy(Compiled.Bytes.data(), &PackedBL, sizeof(PackedBL));
  CompiledSection Code;
  Code.Name = ".text";
  Code.VA = 0x2000;
  Code.Size = Compiled.Bytes.size();
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  CompiledFixupReference Call;
  Call.Offset = 0;
  Call.Symbol = "thumb_callee";
  Call.IsPCRel = true;
  Call.IsResolved = true;
  Call.BitWidth = 24;
  Call.ResolvedValue = 10; // Exact target 0x200a; BL packed it to 0x2008.
  Code.FixupReferences.push_back(Call);
  Compiled.Sections.push_back(Code);
  Compiled.SourceFunctionOwners.push_back({"arm_caller", "arm_caller", 0x2000});
  Compiled.SourceFunctionOwners.push_back(
      {"thumb_callee", "thumb_callee", 0x200a});
  Compiled.SourceFunctionOriginalVAs["arm_caller"] = 0;
  Compiled.SourceFunctionOriginalVAs["thumb_callee"] = 4;

  std::string Detail;
  ASSERT_TRUE(repairMixedARMInterworkingCalls(Compiled, *Source, Detail))
      << Detail;
  uint32_t BLX = 0;
  std::memcpy(&BLX, Compiled.Bytes.data(), sizeof(BLX));
  EXPECT_EQ(BLX, 0xfb000000u);

  const uint32_t PackedBranch = 0xea000000u;
  std::memcpy(Compiled.Bytes.data(), &PackedBranch, sizeof(PackedBranch));
  Detail.clear();
  EXPECT_FALSE(repairMixedARMInterworkingCalls(Compiled, *Source, Detail));
  EXPECT_NE(Detail.find("interworking veneer"), std::string::npos);
  uint32_t Unchanged = 0;
  std::memcpy(&Unchanged, Compiled.Bytes.data(), sizeof(Unchanged));
  EXPECT_EQ(Unchanged, PackedBranch);
}

} // namespace
