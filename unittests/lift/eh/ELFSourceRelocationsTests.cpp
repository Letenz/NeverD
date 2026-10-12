//===- ELFSourceRelocationsTests.cpp - Runtime pointer publication -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/ELF/ELFSourceRelocations.h"
#include "neverd/object/ELFLayout.h"

#include "llvm/MC/MCFixup.h"

namespace {
using namespace neverd;
using ELF = llvm::object::ELF64LE;

class ELFSourceRelocations : public testing::Test {
protected:
  BinaryImage Original;
  CompiledImage Compiled;
  llvm::LLVMContext Context;
  llvm::Module Module{"pointers", Context};

  void SetUp() override {
    Original.Format = BinaryFormat::ELF;
    Original.Arch = Arch::X64;
    Original.Bits = Bitness::Bits64;
    Original.Raw.resize(0x1000, 0);
    auto *Header = reinterpret_cast<ELF::Ehdr *>(Original.Raw.data());
    std::memcpy(Header->e_ident, llvm::ELF::ElfMagic, 4);
    Header->e_ident[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
    Header->e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
    Header->e_ident[llvm::ELF::EI_VERSION] = llvm::ELF::EV_CURRENT;
    Header->e_type = llvm::ELF::ET_DYN;
    Header->e_machine = llvm::ELF::EM_X86_64;
    Header->e_version = llvm::ELF::EV_CURRENT;
    Header->e_ehsize = sizeof(*Header);
    Header->e_phoff = sizeof(*Header);
    Header->e_phentsize = sizeof(ELF::Phdr);
    Header->e_phnum = 3;
    Header->e_shoff = 0x800;
    Header->e_shentsize = sizeof(ELF::Shdr);
    Header->e_shnum = 5;
    Header->e_shstrndx = 4;
    ELFPhdrFields Load;
    Load.Type = llvm::ELF::PT_LOAD;
    Load.Flags = llvm::ELF::PF_R | llvm::ELF::PF_W;
    Load.VAddr = Load.PAddr = 0x1000;
    Load.FileSz = Load.MemSz = 0x1000;
    Load.Align = 4096;
    writeELFPhdr(Original.Raw.data() + sizeof(*Header), true, Load);
    ELFPhdrFields Dynamic;
    Dynamic.Type = llvm::ELF::PT_DYNAMIC;
    Dynamic.VAddr = 0x1200;
    Dynamic.Offset = 0x200;
    Dynamic.FileSz = Dynamic.MemSz = 8 * sizeof(ELF::Dyn);
    writeELFPhdr(Original.Raw.data() + sizeof(*Header) + sizeof(ELF::Phdr),
                 true, Dynamic);
    size_t Index = 0;
    for (auto [Tag, Value] : std::vector<std::pair<int64_t, uint64_t>>{
             {llvm::ELF::DT_SYMTAB, 0x1300},
             {llvm::ELF::DT_SYMENT, sizeof(ELF::Sym)},
             {llvm::ELF::DT_STRTAB, 0x1380},
             {llvm::ELF::DT_STRSZ, 9},
             {llvm::ELF::DT_RELA, 0x1400},
             {llvm::ELF::DT_RELASZ, sizeof(ELF::Rela)},
             {llvm::ELF::DT_RELAENT, sizeof(ELF::Rela)},
             {llvm::ELF::DT_NULL, 0}}) {
      ELF::Dyn Item;
      Item.d_tag = Tag;
      Item.d_un.d_val = Value;
      std::memcpy(Original.Raw.data() + 0x200 + Index++ * sizeof(Item), &Item,
                  sizeof(Item));
    }
    ELFShdrFields Symbols;
    Symbols.Type = llvm::ELF::SHT_DYNSYM;
    Symbols.Addr = 0x1300;
    Symbols.Offset = 0x300;
    Symbols.Size = 2 * sizeof(ELF::Sym);
    Symbols.Link = 2;
    Symbols.EntSize = sizeof(ELF::Sym);
    writeELFShdr(Original.Raw.data() + 0x800 + sizeof(ELF::Shdr), true,
                 Symbols);
    ELF::Sym Symbol{};
    Symbol.st_name = 1;
    Symbol.setBindingAndType(llvm::ELF::STB_GLOBAL, llvm::ELF::STT_FUNC);
    std::memcpy(Original.Raw.data() + 0x300 + sizeof(Symbol), &Symbol,
                sizeof(Symbol));
    std::memcpy(Original.Raw.data() + 0x380, "\0runtime\0", 9);
    ELFShdrFields Strings;
    Strings.Type = llvm::ELF::SHT_STRTAB;
    Strings.Addr = 0x1380;
    Strings.Offset = 0x380;
    Strings.Size = 9;
    writeELFShdr(Original.Raw.data() + 0x800 + 2 * sizeof(ELF::Shdr), true,
                 Strings);
    ELFShdrFields Relocations;
    Relocations.Type = llvm::ELF::SHT_RELA;
    Relocations.Addr = 0x1400;
    Relocations.Offset = 0x400;
    Relocations.Size = Relocations.EntSize = sizeof(ELF::Rela);
    Relocations.Link = 1;
    writeELFShdr(Original.Raw.data() + 0x800 + 3 * sizeof(ELF::Shdr), true,
                 Relocations);
    ELF::Rela Existing{};
    Existing.r_offset = 0x1500;
    Existing.r_addend = 0x1234;
    Existing.setSymbolAndType(0, llvm::ELF::R_X86_64_RELATIVE, false);
    std::memcpy(Original.Raw.data() + 0x400, &Existing, sizeof(Existing));
    Strings.Addr = 0;
    Strings.Offset = 0x500;
    Strings.Size = 1;
    writeELFShdr(Original.Raw.data() + 0x800 + 4 * sizeof(ELF::Shdr), true,
                 Strings);

    Compiled.Success = true;
    Compiled.InputKind = RewriteInputKind::ReplacementIR;
    Compiled.BaseVA = 0x3000;
    Compiled.Bytes.resize(0x1008, 0);
    CompiledSection Code;
    Code.Name = ".text";
    Code.VA = 0x3000;
    Code.Size = 8;
    Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
    Compiled.Sections.push_back(Code);
    CompiledSection Data;
    Data.Name = ".data";
    Data.VA = 0x4000;
    Data.Offset = 0x1000;
    Data.Size = 8;
    Data.Kind = llvm::mc_rewrite::RewriteSectionKind::WritableData;
    CompiledFixupReference Fixup;
    Fixup.Kind = llvm::FK_Data_8;
    Fixup.BitWidth = 64;
    Fixup.IsResolved = true;
    Fixup.Symbol = "runtime";
    Data.FixupReferences.push_back(Fixup);
    Compiled.Sections.push_back(Data);
  }
};

TEST_F(ELFSourceRelocations,
       PreservesOldRelocationsAndPublishesExistingImport) {
  auto Table = prepareELFSourceRelocations(Original, Module, Compiled, 16);
  ASSERT_TRUE(bool(Table)) << llvm::toString(Table.takeError());
  EXPECT_EQ(Table->VA, 0x3010u);
  EXPECT_EQ(Table->Size, 2 * sizeof(ELF::Rela));
  EXPECT_EQ(std::memcmp(Compiled.Bytes.data() + 16, Original.Raw.data() + 0x400,
                        sizeof(ELF::Rela)),
            0);
  ELF::Rela Added;
  std::memcpy(&Added, Compiled.Bytes.data() + 16 + sizeof(Added),
              sizeof(Added));
  EXPECT_EQ(Added.getSymbol(false), 1u);
  EXPECT_EQ(Added.getType(false), llvm::ELF::R_X86_64_64);
  EXPECT_EQ(Added.r_offset, 0x4000u);

  auto Candidate = Original.Raw;
  Candidate.insert(Candidate.end(), Compiled.Bytes.begin(),
                   Compiled.Bytes.end());
  ELFPhdrFields Load;
  Load.Type = llvm::ELF::PT_LOAD;
  Load.VAddr = 0x3000;
  Load.Offset = 0x1000;
  Load.FileSz = Load.MemSz = Compiled.Bytes.size();
  Load.Flags = llvm::ELF::PF_R;
  writeELFPhdr(Candidate.data() + sizeof(ELF::Ehdr) + 2 * sizeof(ELF::Phdr),
               true, Load);
  auto Error = installELFSourceRelocations(Candidate, *Table);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  const auto Section =
      readELFShdr(Candidate.data() + Table->SectionHeaderOffset, true);
  EXPECT_EQ(Section.Addr, 0x3010u);
  EXPECT_EQ(Section.Offset, 0x1010u);
  EXPECT_EQ(Section.Size, 48u);
}

TEST_F(ELFSourceRelocations, LocalPointersUseLoadBiasInsteadOfSymbolLookup) {
  Compiled.SymbolAddrs["runtime"] = 0x3000;
  Compiled.Sections.back().FixupReferences.front().ResolvedValue = 0x3007;
  auto Table = prepareELFSourceRelocations(Original, Module, Compiled, 16);
  ASSERT_TRUE(bool(Table)) << llvm::toString(Table.takeError());
  ELF::Rela Added;
  std::memcpy(&Added, Compiled.Bytes.data() + 16 + sizeof(Added),
              sizeof(Added));
  EXPECT_EQ(Added.getSymbol(false), 0u);
  EXPECT_EQ(Added.getType(false), llvm::ELF::R_X86_64_RELATIVE);
  EXPECT_EQ(Added.r_addend, 0x3007);
}

TEST_F(ELFSourceRelocations,
       RejectsReadonlyPointersAndMissingSymbolsWithoutMutation) {
  const auto Before = Compiled.Bytes;
  Compiled.Sections.back().Kind =
      llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData;
  auto Readonly = prepareELFSourceRelocations(Original, Module, Compiled, 16);
  ASSERT_FALSE(bool(Readonly));
  llvm::consumeError(Readonly.takeError());
  EXPECT_EQ(Compiled.Bytes, Before);
  Compiled.Sections.back().Kind =
      llvm::mc_rewrite::RewriteSectionKind::WritableData;
  Compiled.Sections.back().FixupReferences.front().Symbol = "missing";
  auto Missing = prepareELFSourceRelocations(Original, Module, Compiled, 16);
  ASSERT_FALSE(bool(Missing));
  EXPECT_NE(
      llvm::toString(Missing.takeError()).find("no existing dynamic symbol"),
      std::string::npos);
  EXPECT_EQ(Compiled.Bytes, Before);
}

TEST_F(ELFSourceRelocations, RejectsChangedPublicationIdentityWithoutMutation) {
  auto Table = prepareELFSourceRelocations(Original, Module, Compiled, 16);
  ASSERT_TRUE(bool(Table)) << llvm::toString(Table.takeError());
  Table->VA = 0x1400;
  Table->Size = 24;
  auto Candidate = Original.Raw;
  Candidate[Table->AddressTagOffset] = llvm::ELF::DT_NEEDED;
  const auto Before = Candidate;
  auto Error = installELFSourceRelocations(Candidate, *Table);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("identity changed"),
            std::string::npos);
  EXPECT_EQ(Candidate, Before);
}

TEST_F(ELFSourceRelocations, RejectsWrappedMetadataPlacementWithoutMutation) {
  const auto Before = Compiled.Bytes;
  auto Result =
      prepareELFSourceRelocations(Original, Module, Compiled, UINT64_MAX);
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("overflows"),
            std::string::npos);
  EXPECT_EQ(Compiled.Bytes, Before);
}

} // namespace
