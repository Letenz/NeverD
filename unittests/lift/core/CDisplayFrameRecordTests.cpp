//===- CDisplayFrameRecordTests.cpp - Source pointer types ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source pointer and C record display regression cases.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

namespace {
using namespace neverd;

class RecordContext : public NullDebugContext {
public:
  TypeRef Record = NdType::makeNamedRecord("DISPLAY_RECORD", 16);
  explicit RecordContext(bool MissingFieldTypes = false) {
    Record->FieldDisplayNames = {"first", "second"};
    Record->FieldDisplayOffsets = {0, 8};
    Record->FieldDisplayTypes = {NdType::makeInt(8, false),
                                 NdType::makeInt(8, false)};
    Record->FieldDisplayAlignment = 8;
    if (MissingFieldTypes)
      Record->FieldDisplayTypes = {nullptr, nullptr};
  }
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    if (Name != "borrow_record" && Name != "opaque_frame")
      return std::nullopt;
    FunctionSym F;
    F.Name = Name.str();
    F.ReturnType = NdType::makeVoid();
    F.Params = {{"value", Name == "borrow_record" ? NdType::makePtr(Record)
                                                  : NdType::makeInt(8, false)}};
    F.PrototypeFromHeader = true;
    F.PrototypeLibrary = "frame-record-fixture";
    F.PlatformConvention = true;
    return F;
  }
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    const char *Name = Address == 0x2000   ? "opaque_frame"
                       : Address == 0x3000 ? "borrow_record"
                                           : nullptr;
    if (!Name)
      return std::nullopt;
    auto F = resolvePrototype(Name);
    F->Addr = Address;
    return F;
  }
  bool hasInfo() const override { return true; }
};

void executeView(llvm::StringRef Text) {
  const auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Compiler) << "clang is required for C layout execution";
  llvm::SmallString<128> Source, Program, Errors;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-frame-record", "c", Source));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-record", "exe",
                                                  Program));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-frame-record", "err", Errors));
  llvm::FileRemover RemoveSource(Source), RemoveProgram(Program),
      RemoveErrors(Errors);
  std::error_code EC;
  llvm::raw_fd_ostream Out(Source, EC);
  ASSERT_FALSE(EC);
  Out << "#include <stdint.h>\n"
         "typedef struct DISPLAY_RECORD { uint64_t first,second; } "
         "DISPLAY_RECORD;\n"
         "void opaque_frame(uint64_t); void borrow_record(DISPLAY_RECORD *);\n"
      << Text << R"(
static uintptr_t base_seen;
static int layout_ok;
void opaque_frame(uint64_t base) { base_seen=(uintptr_t)base; }
void borrow_record(DISPLAY_RECORD *p) {
  layout_ok=((uintptr_t)p+16==base_seen && (uintptr_t)p%8==0);
  p->first=7; p->second=11;
}
int main(void) { record_view(); return !layout_ok; }
)";
  Out.close();
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, Errors.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef, 12> Args{*Compiler,
                                                "-std=c11",
                                                Optimization,
                                                "-fno-strict-aliasing",
                                                "-fsanitize=undefined",
                                                "-fsanitize-trap=undefined",
                                                Source,
                                                "-o",
                                                Program};
    auto Result =
        llvm::sys::ExecuteAndWait(*Compiler, Args, std::nullopt, Redirects, 30);
    auto Diagnostic = llvm::MemoryBuffer::getFile(Errors);
    ASSERT_EQ(Result, 0) << (Diagnostic ? (*Diagnostic)->getBuffer().str() : "")
                         << Text.str();
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Program, {Program}, std::nullopt,
                                        Redirects, 10),
              0)
        << Text.str();
  }
}

TEST(CDisplayFrameRecord, BothWritersKeepRecordAddressAndFrameOffsets) {
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    const bool LLVM = Variant & 1;
    SCOPED_TRACE(LLVM);
    SCOPED_TRACE(Variant);
    RecordContext Debug(Variant & 2);
    EXPECT_TRUE(sourceAggregateMembers(Debug.Record).empty());
    EXPECT_EQ(Debug.Record->Alignment, 0u);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = BinaryFormat::COFF;
    Options.EmitIncludes = false;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    if (LLVM) {
      llvm::LLVMContext C;
      llvm::Module M("record-view", C);
      M.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(C);
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(B.getInt64Ty(), false),
          llvm::GlobalValue::ExternalLinkage, "record_view", M);
      rewrite_source::setOriginalVA(*F, 0x1000);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 152),
                                   nullptr, "frame");
      auto *Base = B.CreatePtrToInt(
          B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(72), "frame_end"),
          B.getInt64Ty(), "rsp_init");
      auto Opaque =
          M.getOrInsertFunction("opaque_frame", B.getVoidTy(), B.getInt64Ty());
      auto Borrow =
          M.getOrInsertFunction("borrow_record", B.getVoidTy(), B.getPtrTy());
      rewrite_source::setOriginalVA(
          *llvm::cast<llvm::Function>(Opaque.getCallee()), 0x2000);
      rewrite_source::setOriginalVA(
          *llvm::cast<llvm::Function>(Borrow.getCallee()), 0x3000);
      B.CreateCall(Opaque, {Base});
      B.CreateCall(Borrow, {B.CreateIntToPtr(B.CreateSub(Base, B.getInt64(16)),
                                             B.getPtrTy())});
      B.CreateRet(B.getInt64(0));
      ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options, &Debug, nullptr, F));
    } else {
      HighFunc F;
      F.Name = "record_view";
      F.Entry = 0x1000;
      F.FrameSize = 64;
      F.ReturnType = NdType::makeInt(8, false);
      MedVar SP;
      SP.Kind = MedVar::Reg;
      SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
      SP.Size = 8;
      SP.TheArch = Arch::X64;
      auto Base = HighExpr::makeVar(SP, NdType::makeInt(8, false));
      HighStmt Opaque, Borrow, Return;
      Opaque.Kind = Borrow.Kind = StmtKind::Call;
      Opaque.CallExpr = HighExpr::makeCall("opaque_frame", 0x2000, {Base});
      Borrow.CallExpr =
          HighExpr::makeCall("borrow_record", 0x3000,
                             {HighExpr::makeBinop(NdOp::INT_SUB, Base,
                                                  HighExpr::makeConst(16, 8))});
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeConst(0, 8);
      F.Body = {Opaque, Borrow, Return};
      ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options, &Debug));
    }
    EXPECT_NE(Text.find("DISPLAY_RECORD var_"), std::string::npos) << Text;
    executeView(Text);
  }
}
} // namespace
