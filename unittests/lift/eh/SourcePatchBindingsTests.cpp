//===- SourcePatchBindingsTests.cpp - Original-reference admission --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/SourcePatchBindings.h"
#include "neverd/loader/BinaryImage.h"

namespace {
using namespace neverd;

class SourcePatchBindings : public testing::Test {
protected:
  llvm::LLVMContext Context;
  llvm::Module Module{"references", Context};
  BinaryImage Image;

  void SetUp() override {
    Module.setDataLayout("e-p:64:64-i64:64");
    Image.Arch = Arch::X64;
    Image.Format = BinaryFormat::ELF;
    for (bool Code : {false, true}) {
      Segment Segment;
      Segment.Name = Code ? "text" : "data";
      Segment.VA = Code ? 0x2000 : 0x1000;
      Segment.Size = 0x100;
      Segment.Flags = SegmentFlags::Readable | (Code ? SegmentFlags::Executable
                                                     : SegmentFlags::Writable);
      Segment.Data.resize(Segment.Size, 0);
      Image.Segments.push_back(Segment);
      Section Section;
      Section.Name = Code ? ".text" : ".data";
      Section.VA = Segment.VA;
      Section.Size = Segment.Size;
      Section.Flags = Segment.Flags;
      Image.Sections.push_back(Section);
    }
    Symbol Callee;
    Callee.Name = "original_callee";
    Callee.Addr = 0x2000;
    Callee.Size = 16;
    Callee.IsFunc = true;
    Image.Symbols.push_back(Callee);
    llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "callee", Module);
    for (bool ReadOnly : {false, true})
      new llvm::GlobalVariable(Module, llvm::Type::getInt32Ty(Context),
                               ReadOnly, llvm::GlobalValue::ExternalLinkage,
                               nullptr, ReadOnly ? "constant" : "counter");
  }
};

TEST_F(SourcePatchBindings, BindsDeclarationsToOriginalStorageAndCode) {
  auto Bound = bindSourcePatchReferences(
      Module, Image, {},
      {{"callee", SourcePatchBindingKind::Function, 0x2000},
       {"counter", SourcePatchBindingKind::WritableData, 0x1000},
       {"constant", SourcePatchBindingKind::ReadOnlyData, 0x1004}});
  ASSERT_TRUE(bool(Bound)) << llvm::toString(Bound.takeError());
  ASSERT_EQ(Bound->size(), 3u);
  EXPECT_EQ((*Bound)[1].Size, 4u);
  EXPECT_EQ((*Bound)[2].Size, 4u);
  for (const auto &Binding : *Bound) {
    EXPECT_EQ(Module.getNamedValue(Binding.Symbol), nullptr);
    auto *Value = Module.getNamedValue(makeNdDataSymbol(Binding.OriginalVA));
    ASSERT_NE(Value, nullptr);
    EXPECT_TRUE(Value->isDeclaration());
    EXPECT_TRUE(Value->isDSOLocal());
  }
}

TEST_F(SourcePatchBindings, InvalidSecondBindingDoesNotChangeFirstDeclaration) {
  for (uint64_t Address :
       {uint64_t(0x1001), uint64_t(0x10ff), uint64_t(0x3000)}) {
    auto Bound = bindSourcePatchReferences(
        Module, Image, {},
        {{"callee", SourcePatchBindingKind::Function, 0x2000},
         {"counter", SourcePatchBindingKind::WritableData, Address}});
    EXPECT_FALSE(bool(Bound));
    llvm::consumeError(Bound.takeError());
    EXPECT_NE(Module.getFunction("callee"), nullptr);
    EXPECT_NE(Module.getNamedValue("counter"), nullptr);
    EXPECT_EQ(Module.getNamedValue(makeNdDataSymbol(0x2000)), nullptr);
  }
}

TEST_F(SourcePatchBindings, CannotAliasAReplacementAsItsOriginalBody) {
  auto Bound = bindSourcePatchReferences(
      Module, Image, {{"replacement", 0x2000}},
      {{"callee", SourcePatchBindingKind::Function, 0x2000}});
  ASSERT_FALSE(bool(Bound));
  EXPECT_NE(llvm::toString(Bound.takeError()).find("original-body callbacks"),
            std::string::npos);
}

TEST_F(SourcePatchBindings, RejectsWritableDeclarationsForReadOnlyStorage) {
  auto ConstMismatch = bindSourcePatchReferences(
      Module, Image, {},
      {{"counter", SourcePatchBindingKind::ReadOnlyData, 0x1000}});
  ASSERT_FALSE(bool(ConstMismatch));
  EXPECT_NE(llvm::toString(ConstMismatch.takeError()).find("const declaration"),
            std::string::npos);
  Image.Sections.front().Flags = SegmentFlags::Readable;
  auto PermissionMismatch = bindSourcePatchReferences(
      Module, Image, {},
      {{"counter", SourcePatchBindingKind::WritableData, 0x1000}});
  ASSERT_FALSE(bool(PermissionMismatch));
  EXPECT_NE(llvm::toString(PermissionMismatch.takeError()).find("not writable"),
            std::string::npos);
}

} // namespace
