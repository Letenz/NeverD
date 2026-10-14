//===- ELFSourceSymbolsTests.cpp - Original import use contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/ELF/ELFSourceSymbols.h"

#include "llvm/BinaryFormat/ELF.h"

namespace {
using namespace neverd;

class ELFSourceSymbols : public testing::Test {
protected:
  llvm::LLVMContext Context;
  llvm::Module Module{"source", Context};
  BinaryImage Image;
  llvm::mc_rewrite::RewriteSymbolResolveRequest Request;

  void SetUp() override {
    Image.Format = BinaryFormat::ELF;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    for (bool Code : {true, false}) {
      Segment Segment;
      Segment.VA = Code ? 0x1000 : 0x2000;
      Segment.Size = 256;
      Segment.Flags = SegmentFlags::Readable | (Code ? SegmentFlags::Executable
                                                     : SegmentFlags::Writable);
      Segment.Data.resize(Segment.Size, 0);
      Image.Segments.push_back(Segment);
      Section Section;
      Section.VA = Segment.VA;
      Section.Size = Segment.Size;
      Section.Flags = Segment.Flags;
      Image.Sections.push_back(Section);
    }
    Import Import;
    Import.Name = "puts";
    Import.IATAddr = 0x2000;
    Image.Imports.push_back(Import);
    ASSERT_TRUE(Image.recordImportStub(0x1000, 0));
    llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "puts", Module);
    RelocationEntry Relocation;
    Relocation.SymbolName = "puts";
    Relocation.Address = 0x2008;
    Relocation.Type = llvm::ELF::R_X86_64_GLOB_DAT;
    Image.Relocations.push_back(Relocation);
    Request.Symbol = "puts";
    Request.SpecifierName = "PLT";
    Request.Specifier = 19;
  }

  std::optional<uint64_t> resolve() {
    auto Result = resolveELFSourceSymbol(Image, Module, Request);
    EXPECT_TRUE(bool(Result));
    if (!Result) {
      ADD_FAILURE() << llvm::toString(Result.takeError());
      return std::nullopt;
    }
    return *Result;
  }
};

TEST_F(ELFSourceSymbols, CallsAndGOTLoadsResolveToDifferentAddresses) {
  EXPECT_EQ(resolve(), 0x1000u);
  Request.SpecifierName = "GOTPCREL";
  Request.Specifier = 11;
  EXPECT_EQ(resolve(), 0x2008u);
}

TEST_F(ELFSourceSymbols, AArch64PageAndOffsetUseTheAuthenticatedGOTSlot) {
  Image.Arch = Arch::AArch64;
  Image.Relocations.front().Type = llvm::ELF::R_AARCH64_GLOB_DAT;
  for (auto [Name, Value] :
       {std::pair{"GOT_PAGE", 0x014u}, {"GOT_LO12", 0x124u}}) {
    Request.SpecifierName = Name;
    Request.Specifier = Value;
    EXPECT_EQ(resolve(), 0x2008u);
  }
  Image.Relocations.front().Type = llvm::ELF::R_AARCH64_JUMP_SLOT;
  EXPECT_EQ(resolve(), std::nullopt);
  Request.Specifier = 0x034; // TLS is not an ordinary GOT slot.
  Request.SpecifierName = "GOTTPREL_PAGE";
  auto TLS = resolveELFSourceSymbol(Image, Module, Request);
  ASSERT_FALSE(bool(TLS));
  EXPECT_NE(llvm::toString(TLS.takeError()).find("unsupported relocation"),
            std::string::npos);
}

TEST_F(ELFSourceSymbols, AArch64AbsoluteFormsRequireAnExplicitDataBinding) {
  Image.Arch = Arch::AArch64;
  Request.Symbol = "__nd_data_2000";
  auto *Data = new llvm::GlobalVariable(
      Module, llvm::Type::getInt32Ty(Context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, Request.Symbol);
  Data->setDSOLocal(true);
  for (auto [Name, Value] :
       {std::pair{"ABS", 0x001u}, {"ABS_PAGE", 0x011u}, {"ABS_LO12", 0x121u}}) {
    Request.SpecifierName = Name;
    Request.Specifier = Value;
    EXPECT_EQ(resolve(), 0x2000u);
  }
  Request.Specifier = 0x999;
  auto Unknown = resolveELFSourceSymbol(Image, Module, Request);
  ASSERT_FALSE(bool(Unknown));
  EXPECT_NE(llvm::toString(Unknown.takeError()).find("unsupported relocation"),
            std::string::npos);
  Request.Specifier = 0x121;
  Data->setDSOLocal(false);
  auto Unbound = resolveELFSourceSymbol(Image, Module, Request);
  ASSERT_FALSE(bool(Unbound));
  llvm::consumeError(Unbound.takeError());
}

TEST_F(ELFSourceSymbols, MissingPLTDoesNotReturnTheGOTDataSlot) {
  Image.ImportStubIndices.clear();
  EXPECT_EQ(resolve(), std::nullopt);
}

TEST_F(ELFSourceSymbols, MissingGOTDoesNotReturnCodeOrLazyResolver) {
  Request.SpecifierName = "GOTPCREL";
  Image.Relocations.front().Type = llvm::ELF::R_X86_64_JUMP_SLOT;
  EXPECT_EQ(resolve(), std::nullopt);
}

TEST_F(ELFSourceSymbols, MultipleExactEntrypointsAreAmbiguous) {
  ASSERT_TRUE(Image.recordImportStub(0x1010, 0));
  auto Result = resolveELFSourceSymbol(Image, Module, Request);
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("ambiguous"),
            std::string::npos);
}

TEST_F(ELFSourceSymbols, RejectsTLSAndConflictingSlotWriters) {
  Request.SpecifierName = "TLSGD";
  auto TLS = resolveELFSourceSymbol(Image, Module, Request);
  ASSERT_FALSE(bool(TLS));
  llvm::consumeError(TLS.takeError());
  Request.SpecifierName = "GOTPCREL";
  RelocationEntry Other = Image.Relocations.front();
  Other.SymbolName = "different_symbol";
  Image.Relocations.push_back(Other);
  auto Conflict = resolveELFSourceSymbol(Image, Module, Request);
  ASSERT_FALSE(bool(Conflict));
  EXPECT_NE(llvm::toString(Conflict.takeError()).find("conflicting"),
            std::string::npos);
}

TEST(ELFSourceProperties,
     RetainsCETCompatibilityAndRejectsUnknownRequirements) {
  std::vector<uint8_t> Note(32, 0);
  writeLE<uint32_t>(Note.data(), 4);
  writeLE<uint32_t>(Note.data() + 4, 16);
  writeLE<uint32_t>(Note.data() + 8, llvm::ELF::NT_GNU_PROPERTY_TYPE_0);
  std::memcpy(Note.data() + 12, "GNU\0", 4);
  writeLE<uint32_t>(Note.data() + 16,
                    llvm::ELF::GNU_PROPERTY_X86_FEATURE_1_AND);
  writeLE<uint32_t>(Note.data() + 20, 4);
  writeLE<uint32_t>(Note.data() + 24, 3);
  auto Features = readELFX64PatchPropertyNote(Note);
  ASSERT_TRUE(bool(Features));
  EXPECT_EQ(*Features, 3u);
  writeLE<uint32_t>(Note.data() + 24, 8);
  auto Unknown = readELFX64PatchPropertyNote(Note);
  ASSERT_FALSE(bool(Unknown));
  EXPECT_NE(llvm::toString(Unknown.takeError()).find("unsupported"),
            std::string::npos);
  Note.resize(20);
  auto Truncated = readELFX64PatchPropertyNote(Note);
  ASSERT_FALSE(bool(Truncated));
  llvm::consumeError(Truncated.takeError());
}

TEST_F(ELFSourceSymbols, ELF32GNUPropertiesUseFourByteDescriptorAlignment) {
  std::vector<uint8_t> Note(28, 0);
  writeLE<uint32_t>(Note.data(), 4);
  writeLE<uint32_t>(Note.data() + 4, 12);
  writeLE<uint32_t>(Note.data() + 8, llvm::ELF::NT_GNU_PROPERTY_TYPE_0);
  std::memcpy(Note.data() + 12, "GNU\0", 4);
  writeLE<uint32_t>(Note.data() + 16, llvm::ELF::GNU_PROPERTY_X86_ISA_1_NEEDED);
  writeLE<uint32_t>(Note.data() + 20, 4);
  writeLE<uint32_t>(Note.data() + 24, 1);
  auto Good = readELFPatchPropertyNote(Note, 4);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(*Good, 0u);
  auto Bad = readELFPatchPropertyNote(Note, 8);
  EXPECT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
}

} // namespace
