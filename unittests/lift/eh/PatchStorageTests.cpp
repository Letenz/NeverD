//===- PatchStorageTests.cpp - Permission groups for generated data ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/PatchStorage.h"

#include "llvm/IR/IRBuilder.h"

namespace {
using namespace neverd;

TEST(PatchStorage, GeneratedPersistentStateOccupiesPagesAfterCode) {
  for (auto Format : {BinaryFormat::ELF, BinaryFormat::MachO}) {
    llvm::LLVMContext Context;
    llvm::Module Module("counter", Context);
    auto *I32 = llvm::Type::getInt32Ty(Context);
    auto *Counter = new llvm::GlobalVariable(
        Module, I32, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(I32, 5), "counter");
    auto *Function = llvm::Function::Create(llvm::FunctionType::get(I32, false),
                                            llvm::GlobalValue::ExternalLinkage,
                                            "replacement", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Next = Builder.CreateAdd(Builder.CreateLoad(I32, Counter),
                                   llvm::ConstantInt::get(I32, 1));
    Builder.CreateStore(Next, Counter);
    Builder.CreateRet(Next);
    const Arch ISA = Format == BinaryFormat::ELF ? Arch::X64 : Arch::AArch64;
    auto Image = compileImageForPatch(
        Module, ISA, Format, 0x104310,
        [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
          return std::nullopt;
        },
        0, {}, RewriteInputKind::ReplacementIR);
    ASSERT_TRUE(Image.Success);
    auto Layout = validatePatchStorageLayout(
        Image, sourcePatchStorageAlignment(ISA, Format));
    ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
    ASSERT_GT(Layout->WritableOffset, 0u);
    const uint64_t Boundary = Image.BaseVA + Layout->WritableOffset;
    for (const auto &Section : Image.Sections) {
      if (!Section.IsAllocated || !Section.IsInImage || !Section.Size)
        continue;
      if (Section.Kind == llvm::mc_rewrite::RewriteSectionKind::Code)
        EXPECT_LE(Section.VA + Section.Size, Boundary);
      if (Section.Kind == llvm::mc_rewrite::RewriteSectionKind::WritableData)
        EXPECT_GE(Section.VA, Boundary);
    }
  }
}

TEST(PatchStorage, RejectsDataSharingAnExecutablePage) {
  CompiledImage Image;
  Image.Success = true;
  Image.BaseVA = 0x1000;
  Image.Bytes.resize(64);
  CompiledSection Code;
  Code.Name = ".text";
  Code.VA = Image.BaseVA;
  Code.Size = 32;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  Image.Sections.push_back(Code);
  CompiledSection Data;
  Data.Name = ".data";
  Data.VA = Image.BaseVA + 32;
  Data.Offset = 32;
  Data.Size = 4;
  Data.Kind = llvm::mc_rewrite::RewriteSectionKind::WritableData;
  Image.Sections.push_back(Data);
  auto Layout = validatePatchStorageLayout(Image, 4096);
  ASSERT_FALSE(bool(Layout));
  EXPECT_NE(llvm::toString(Layout.takeError()).find("whole pages"),
            std::string::npos);
}

} // namespace
