//===- ReplacementIRTests.cpp - Frontend runtime requirement admission ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/CodeGen.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>

namespace {

class ReplacementIR : public testing::Test {
protected:
  llvm::LLVMContext Context;
  llvm::Module Module{"source", Context};
  llvm::Function *Function = nullptr;

  void SetUp() override {
    Module.setTargetTriple(llvm::Triple("aarch64-unknown-linux-gnu"));
    Function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "replacement", Module);
    llvm::IRBuilder<>(llvm::BasicBlock::Create(Context, "entry", Function))
        .CreateRetVoid();
  }

  std::string error() {
    return llvm::toString(
        neverd::validateReplacementIR(Module, neverd::Arch::AArch64));
  }
};

TEST_F(ReplacementIR, RejectsUnsupportedFunctionAndModuleStackProbeMethods) {
  Function->addFnAttr("probe-stack", "__chkstk_darwin");
  EXPECT_NE(error().find("unsupported AArch64 stack probe"), std::string::npos);
  EXPECT_TRUE(Function->hasFnAttribute("probe-stack"));
  Function->removeFnAttr("probe-stack");
  Module.addModuleFlag(llvm::Module::Error, "probe-stack",
                       llvm::MDString::get(Context, "__chkstk_darwin"));
  EXPECT_NE(error().find("unsupported AArch64 stack probe"), std::string::npos);
  Function->addFnAttr("probe-stack", "inline-asm");
  EXPECT_EQ(error(), "");
  EXPECT_EQ(Function->getFnAttribute("probe-stack").getValueAsString(),
            "inline-asm");
}

TEST_F(ReplacementIR, RejectsInvalidProbeSizesBeforeBackendAssertions) {
  for (llvm::StringRef Value : {"0", "-1", "bad", "9223372036854775808"}) {
    Function->addFnAttr("stack-probe-size", Value);
    EXPECT_NE(error().find("invalid stack-probe-size"), std::string::npos);
  }
  Function->removeFnAttr("stack-probe-size");
  Module.addModuleFlag(llvm::Module::Error, "stack-probe-size", 0u);
  EXPECT_NE(error().find("invalid module stack-probe-size"), std::string::npos);
}

TEST_F(ReplacementIR, CannotExternalizeUserDefinedReservedData) {
  auto *Global = new llvm::GlobalVariable(
      Module, llvm::Type::getInt32Ty(Context), true,
      llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 17),
      "__nd_data_1000");
  EXPECT_NE(error().find("reserved original-data symbol"), std::string::npos);
  EXPECT_TRUE(Global->hasInitializer());
  Global->setInitializer(nullptr);
  EXPECT_EQ(error(), "");
}

TEST(ReplacementNativeCode, ExternalPLTReferenceMustBeExactBeforePublication) {
  llvm::LLVMContext Context;
  llvm::Module Module("external-call", Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), false);
  auto *Caller = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "replacement", Module);
  Caller->addFnAttr("disable-tail-calls", "true");
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, "entry", Caller));
  // Force the actual PLT expression: CodeGen marks IR function declarations
  // dso_local, but late compiler libcalls (including _Unwind_Resume) still
  // carry PLT. Inline asm isolates precisely that MC addressing contract.
  auto *AsmType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  Builder.CreateCall(
      llvm::InlineAsm::get(AsmType, "callq external_plt@PLT", "", true));
  Builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0));
  constexpr uint64_t TargetVA = 0x1134;
  // The supported r3 prebuilt is known to encode this particular expression
  // incorrectly. Probe the producer independently, then require either the
  // exact branch or fail-closed publication. Integrated LLVM validation also
  // requires the corrected producer through the explicit test environment.
  llvm::mc_rewrite::RewriteOptions Probe;
  Probe.Model.TextVA = 0x5320;
  Probe.Model.resolve = [TargetVA](llvm::StringRef Symbol,
                                   uint32_t) -> std::optional<uint64_t> {
    return Symbol == "external_plt" ? std::optional<uint64_t>(TargetVA)
                                    : std::nullopt;
  };
  std::string SectionName;
  uint64_t FieldOffset = 0, FieldVA = 0;
  int64_t Addend = 0;
  unsigned ProbeCalls = 0;
  Probe.onFixup = [&](const llvm::mc_rewrite::FixupCtx &Fixup, uint64_t Value) {
    if (Fixup.Sym == "external_plt") {
      ++ProbeCalls;
      SectionName = Fixup.SectionName.str();
      FieldOffset = Fixup.SectionOffset;
      FieldVA = Fixup.FixupVA;
      Addend = Fixup.Addend;
    }
    return Value;
  };
  auto ProbeModule = llvm::CloneModule(Module);
  neverd::Codegen Generator;
  const auto Raw = Generator.compileForRewrite(
      *ProbeModule, neverd::Arch::X64, Probe, neverd::BinaryFormat::ELF, {},
      neverd::RewriteInputKind::ReplacementIR);
  ASSERT_TRUE(Raw.ImageValid);
  ASSERT_TRUE(Raw.Unresolved.empty());
  ASSERT_EQ(ProbeCalls, 1u);
  const auto Section = llvm::find_if(Raw.Sections, [&](const auto &Candidate) {
    return Candidate.Name == SectionName;
  });
  ASSERT_NE(Section, Raw.Sections.end());
  ASSERT_LE(FieldOffset + 4, Section->Bytes.size());
  const auto Encoded =
      neverd::readLE<int32_t>(Section->Bytes.data() + FieldOffset);
  const bool ExactProducer = FieldVA + 4 + Encoded == TargetVA + Addend;
  if (std::getenv("NEVERD_TEST_REQUIRE_FIXED_LLVM"))
    EXPECT_TRUE(ExactProducer)
        << "integrated LLVM still has the PLT/page collision";
  auto Compiled = neverd::compileImageForPatch(
      Module, neverd::Arch::X64, neverd::BinaryFormat::ELF, 0x5320,
      [TargetVA](const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request)
          -> std::optional<uint64_t> {
        if (Request.Symbol == "external_plt")
          return TargetVA;
        return std::nullopt;
      },
      0, {}, neverd::RewriteInputKind::ReplacementIR);
  ASSERT_EQ(Compiled.Success, ExactProducer);
  if (!Compiled.Success)
    return;
  unsigned Calls = 0;
  for (const auto &Section : Compiled.Sections)
    for (const auto &Fixup : Section.FixupReferences) {
      if (Fixup.Symbol != "external_plt")
        continue;
      ++Calls;
      ASSERT_TRUE(Fixup.IsPCRel);
      ASSERT_EQ(Fixup.BitWidth, 32u);
      const auto Displacement = neverd::readLE<int32_t>(
          Compiled.Bytes.data() + Section.Offset + Fixup.Offset);
      EXPECT_EQ(Section.VA + Fixup.Offset + 4 + Displacement,
                TargetVA + Fixup.Addend);
    }
  EXPECT_EQ(Calls, 1u);
}

TEST(ReplacementNativeCode, MachOGOTDataReferenceMustUseTheEndOfItsWord) {
  llvm::LLVMContext Context;
  llvm::Module Module("macho-type-info", Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), false);
  auto *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "replacement", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0));
  // This is Clang's LSDA type-table spelling. In a Mach-O object the GOT
  // relocation is PC-relative to the end of the four-byte word; the +4 makes
  // the final type table relative to the field's beginning, as DWARF requires.
  Module.appendModuleInlineAsm(".section __TEXT,__gcc_except_tab\n"
                               ".p2align 2\n.long external_type@GOTPCREL+4\n");
  constexpr uint64_t TargetVA = 0x104018, TextVA = 0x105320;
  auto Resolve = [TargetVA](llvm::StringRef Name,
                            uint32_t) -> std::optional<uint64_t> {
    return Name == "external_type" ? std::optional<uint64_t>(TargetVA)
                                   : std::nullopt;
  };
  llvm::mc_rewrite::RewriteOptions Probe;
  Probe.Model.TextVA = TextVA;
  Probe.Model.resolve = Resolve;
  uint64_t FieldVA = 0, FieldOffset = 0;
  int64_t Addend = 0;
  std::string SectionName;
  unsigned Fields = 0;
  Probe.onFixup = [&](const llvm::mc_rewrite::FixupCtx &Fixup, uint64_t Value) {
    if (Fixup.Sym == "external_type") {
      ++Fields;
      FieldVA = Fixup.FixupVA;
      FieldOffset = Fixup.SectionOffset;
      Addend = Fixup.Addend;
      SectionName = Fixup.SectionName.str();
    }
    return Value;
  };
  auto Copy = llvm::CloneModule(Module);
  neverd::Codegen Generator;
  auto Raw = Generator.compileForRewrite(
      *Copy, neverd::Arch::X64, Probe, neverd::BinaryFormat::MachO, {},
      neverd::RewriteInputKind::ReplacementIR);
  ASSERT_TRUE(Raw.ImageValid);
  ASSERT_TRUE(Raw.Unresolved.empty());
  ASSERT_EQ(Fields, 1u);
  const auto Section = llvm::find_if(
      Raw.Sections, [&](const auto &S) { return S.Name == SectionName; });
  ASSERT_NE(Section, Raw.Sections.end());
  ASSERT_LE(FieldOffset + 4, Section->Bytes.size());
  const auto Delta =
      neverd::readLE<int32_t>(Section->Bytes.data() + FieldOffset);
  const bool ExactProducer = FieldVA + 4 + Delta == TargetVA + Addend;
  if (std::getenv("NEVERD_TEST_REQUIRE_FIXED_LLVM"))
    EXPECT_TRUE(ExactProducer)
        << "LLVM still misanchors Mach-O GOT data by four bytes";
  // Check both appended RO metadata and metadata fixed in an original range.
  for (bool External : {false, true}) {
    auto Compiled = neverd::compileImageForPatchWithFixedSectionVAs(
        Module, neverd::Arch::X64, neverd::BinaryFormat::MachO, TextVA, Resolve,
        [External](llvm::StringRef Name) -> std::optional<uint64_t> {
          return External && Name == "__gcc_except_tab"
                     ? std::optional<uint64_t>(0x108000)
                     : std::nullopt;
        },
        0, {}, neverd::RewriteInputKind::ReplacementIR);
    ASSERT_EQ(Compiled.Success, ExactProducer);
    if (!Compiled.Success)
      continue;
    Fields = 0;
    for (const auto &S : Compiled.Sections)
      for (const auto &Fixup : S.FixupReferences)
        if (Fixup.Symbol == "external_type") {
          ++Fields;
          const auto *Bytes = S.IsInImage ? Compiled.Bytes.data() + S.Offset
                                          : S.ExternalBytes.data();
          const auto Delta = neverd::readLE<int32_t>(Bytes + Fixup.Offset);
          EXPECT_EQ(S.VA + Fixup.Offset + 4 + Delta, TargetVA + Fixup.Addend);
        }
    EXPECT_EQ(Fields, 1u);
  }
}

} // namespace
