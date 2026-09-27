//===- ELFARM32ModeTests.cpp - ELF ARM instruction-mode evidence ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

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

TEST_F(ELFARM32ModeTest, PreservesMixedMetadataButRefusesSingleModeDecoding) {
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
    Decoder Dec;
    EXPECT_FALSE(Dec.init(Image->Arch, Image->Mode));
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
    EXPECT_FALSE(Lifted.ok()) << Lifted.out;
    EXPECT_NE(Lifted.err.find("mixed ARM/Thumb decoding unsupported"),
              std::string::npos)
        << Lifted.err;
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

} // namespace
