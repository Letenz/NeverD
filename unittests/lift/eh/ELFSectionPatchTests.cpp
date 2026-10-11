//===- ELFSectionPatchTests.cpp - ELF patch section ownership tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/ELF/ELFSectionPatch.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/object/ELFLayout.h"

#include "llvm/Support/MemoryBuffer.h"

namespace {
using namespace neverd;

template <class ELF> std::vector<uint8_t> imageWithOnlyUnwindSection() {
  constexpr bool Is64 = sizeof(typename ELF::Ehdr) == 64;
  std::vector<uint8_t> Bytes(0x400, 0);
  auto *Header = reinterpret_cast<typename ELF::Ehdr *>(Bytes.data());
  std::memcpy(Header->e_ident, llvm::ELF::ElfMagic, 4);
  Header->e_ident[llvm::ELF::EI_CLASS] =
      Is64 ? llvm::ELF::ELFCLASS64 : llvm::ELF::ELFCLASS32;
  Header->e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
  Header->e_ident[llvm::ELF::EI_VERSION] = llvm::ELF::EV_CURRENT;
  Header->e_type = llvm::ELF::ET_EXEC;
  Header->e_machine = Is64 ? llvm::ELF::EM_X86_64 : llvm::ELF::EM_386;
  Header->e_version = llvm::ELF::EV_CURRENT;
  Header->e_ehsize = sizeof(*Header);
  Header->e_entry = 0x1100;
  Header->e_phoff = sizeof(*Header);
  Header->e_phentsize = sizeof(typename ELF::Phdr);
  Header->e_phnum = 1;
  Header->e_shoff = 0x300;
  Header->e_shentsize = sizeof(typename ELF::Shdr);
  Header->e_shnum = 3;
  Header->e_shstrndx = 1;
  ELFPhdrFields Segment;
  Segment.Type = llvm::ELF::PT_LOAD;
  Segment.Flags = llvm::ELF::PF_R | llvm::ELF::PF_X;
  Segment.VAddr = Segment.PAddr = 0x1000;
  Segment.FileSz = Segment.MemSz = 0x200;
  Segment.Align = 0x1000;
  writeELFPhdr(Bytes.data() + sizeof(*Header), Is64, Segment);
  constexpr char Names[] = "\0.shstrtab\0.eh_frame_hdr\0";
  std::memcpy(Bytes.data() + 0x240, Names, sizeof(Names));
  ELFShdrFields Strings;
  Strings.Name = 1;
  Strings.Type = llvm::ELF::SHT_STRTAB;
  Strings.Offset = 0x240;
  Strings.Size = sizeof(Names);
  Strings.AddrAlign = 1;
  writeELFShdr(Bytes.data() + 0x300 + sizeof(typename ELF::Shdr), Is64,
               Strings);
  ELFShdrFields Unwind;
  Unwind.Name = 11;
  Unwind.Type = llvm::ELF::SHT_PROGBITS;
  Unwind.Flags = llvm::ELF::SHF_ALLOC;
  Unwind.Addr = 0x1180;
  Unwind.Offset = 0x180;
  Unwind.Size = 0x20;
  Unwind.AddrAlign = 4;
  writeELFShdr(Bytes.data() + 0x300 + 2 * sizeof(typename ELF::Shdr), Is64,
               Unwind);
  return Bytes;
}

CompiledSection codeSection() {
  CompiledSection Code;
  Code.Name = ".text";
  Code.VA = 0x1100;
  Code.Size = 0x20;
  Code.Alignment = 16;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  return Code;
}

TEST(ELFPatchSections, ReloadRecognizesCodeWithoutClassifyingUnwindAsCode) {
  for (auto Bytes : {imageWithOnlyUnwindSection<llvm::object::ELF64LE>(),
                     imageWithOnlyUnwindSection<llvm::object::ELF32LE>()}) {
    auto Read = [&] {
      return ELFLoader().loadBuffer(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                          Bytes.size()),
          "patch-sections"));
    };
    auto Before = Read();
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    EXPECT_FALSE(Before->isCodeAddress(0x1100));
    auto Error = registerELFPatchSections(Bytes, {codeSection()});
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    auto After = Read();
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
    EXPECT_TRUE(After->isCodeRange(0x1100, 0x20));
    EXPECT_FALSE(After->isCodeAddress(0x1120));
    EXPECT_FALSE(After->isCodeAddress(0x1180));
    ELFShdrFields Section;
    ASSERT_TRUE(
        findELFSection(Bytes.data(), Bytes.size(), ".neverd.text", Section));
    EXPECT_EQ(Section.Offset, 0x100u);
    EXPECT_EQ(Section.Addr, 0x1100u);
    ASSERT_TRUE(
        findELFSection(Bytes.data(), Bytes.size(), ".eh_frame_hdr", Section));
    EXPECT_EQ(Section.Addr, 0x1180u);
  }
}

TEST(ELFPatchSections,
     RejectsUnmappedOverlappingOrWrongPermissionWithoutMutation) {
  const auto Original = imageWithOnlyUnwindSection<llvm::object::ELF64LE>();
  for (unsigned Failure = 0; Failure != 5; ++Failure) {
    auto Bytes = Original;
    auto Section = codeSection();
    if (Failure == 0)
      Section.VA = 0x1300;
    else if (Failure == 1)
      Section.VA = 0x1180;
    else if (Failure == 2)
      Section.Kind = llvm::mc_rewrite::RewriteSectionKind::WritableData;
    else if (Failure == 3)
      Section.VA += 1;
    else
      Section.Size = UINT64_MAX;
    auto Error = registerELFPatchSections(Bytes, {Section});
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bytes, Original);
  }
}

} // namespace
