//===- LLVMCScalarLoopRecoveryTests.cpp - Proved source loop integration --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/pass/LLVMC/LLVMCScalarLoopRecovery.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

namespace {
void compileAndRun(const std::string &Source,
                   llvm::StringRef Optimization = "-O2",
                   const std::string &ReferenceIR = {},
                   bool CheckUndefinedBehavior = false) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  llvm::SmallString<128> ReferencePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-recovery", "ll",
                                                  ReferencePath));
  llvm::FileRemover RemoveReference(ReferencePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-recovery", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-recovery",
                                                  "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-recovery",
                                                  "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  if (!ReferenceIR.empty()) {
    llvm::raw_fd_ostream OS(ReferencePath, EC);
    ASSERT_FALSE(EC);
    OS << ReferenceIR;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  llvm::SmallVector<llvm::StringRef, 12> Arguments{Compiler,
                                                   "-std=c11",
                                                   Optimization,
                                                   "-Werror=uninitialized",
                                                   "-Werror=return-type",
                                                   SourcePath,
                                                   "-o",
                                                   BinaryPath};
  if (!ReferenceIR.empty())
    Arguments.push_back(ReferencePath);
  if (CheckUndefinedBehavior) {
    Arguments.push_back("-fsanitize=undefined");
    Arguments.push_back("-fsanitize-trap=all");
  }
  std::string Error;
  int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                         Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Result, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "")
                       << '\n'
                       << Source;
  Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                     Redirects, 30, 0, &Error);
  ASSERT_EQ(Result, 0) << Error << '\n' << Source;
}

// Independently authored arithmetic: a peeled first update and a bottom-tested
// remainder. Every byte control and full-width data remain observable.
constexpr char Prefix[] = R"(
define i32 @accumulate(i32 noundef %seed, i32 noundef %delta, i8 noundef %count) {
entry:
  %bound = and i8 %count, 3
  %empty = icmp eq i8 %bound, 0
  br i1 %empty, label %zero, label %peeled
zero:
  ret i32 %seed
peeled:
  %first = add i32 %seed, %delta
  %one = icmp eq i8 %bound, 1
  br i1 %one, label %exit, label %pre
pre:
  br label %loop
loop:
  %i = phi i8 [1, %pre], [%next, %loop]
  %value = phi i32 [%first, %pre], [%sum, %loop]
  %sum = add i32 %value, %delta
  %next = add nuw i8 %i, 1
  %done = icmp eq i8 %next, %bound
  br i1 %done, label %exit, label %loop
exit:
  %result = phi i32 [%first, %peeled], [%sum, %loop]
  ret i32 %result
}
)";

std::string text(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  Module.print(Out, nullptr);
  return Text;
}

std::unique_ptr<llvm::Module> parse(llvm::StringRef IR,
                                    llvm::LLVMContext &Context) {
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(IR, Error, Context);
  EXPECT_TRUE(Module) << Error.getMessage().str();
  if (Module)
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  return Module;
}

std::string emit(llvm::Module &Module, const llvm::Function *Only = nullptr,
                 const neverd::BinaryImage *Image = nullptr) {
  const auto Before = text(Module);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.Image = Image;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options, nullptr,
                                          nullptr, Only));
  EXPECT_EQ(text(Module), Before);
  return Source;
}

void checkArithmetic(llvm::Module &Module, const std::string &Source) {
  Module.getFunction("accumulate")->setName("reference_accumulate");
  const std::string Reference = text(Module);
  Module.getFunction("reference_accumulate")->setName("accumulate");
  const std::string Main = R"(
extern uint32_t reference_accumulate(uint32_t, uint32_t, uint8_t);
int main(void) {
  const uint32_t values[] = {0, 1, 2, 255, 256, 65535, 65536,
                            0x7fffffffU, 0x80000000U, 0xffffffffU};
  for (unsigned a = 0; a < sizeof(values) / sizeof(values[0]); ++a)
    for (unsigned b = 0; b < sizeof(values) / sizeof(values[0]); ++b)
      for (unsigned n = 0; n < 256; ++n) {
        uint32_t expected = values[a] + values[b] * (n & 3U);
        if (accumulate(values[a], values[b], n) != expected ||
            reference_accumulate(values[a], values[b], n) != expected) return 1;
      }
  uint32_t random = 0x273549afU;
  for (unsigned n = 0; n < 8192; ++n) {
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
    uint32_t a = random;
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
    if (accumulate(a, random, n) != a + random * (n & 3U) ||
        accumulate(a, random, n) != reference_accumulate(a, random, n)) return 2;
  }
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    compileAndRun(Source + Main, Level, Reference, true);
}

TEST(LLVMCScalarLoopRecovery, DefaultEmitterRecoversPeeledLoops) {
  llvm::LLVMContext Context;
  auto Module = parse(Prefix, Context);
  ASSERT_TRUE(Module);
  const auto Source = emit(*Module);
  EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  checkArithmetic(*Module, Source);
}

TEST(LLVMCScalarLoopRecovery, SelectedFunctionUsesTheSameNormalization) {
  llvm::LLVMContext Context;
  auto Module = parse(std::string(Prefix) + R"(
define i32 @unrelated(i32 %x) { %r = add i32 %x, 101 ret i32 %r }
)",
                      Context);
  ASSERT_TRUE(Module);
  const auto Source = emit(*Module, Module->getFunction("accumulate"));
  EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unrelated"), std::string::npos) << Source;
  checkArithmetic(*Module, Source);
}

TEST(LLVMCScalarLoopRecovery,
     PublicationPreservesIdentityAttributesAndCallers) {
  llvm::LLVMContext Context;
  auto Module = parse(std::string(Prefix) + R"(
define i32 @caller(i32 %a, i32 %b, i8 %n) {
  %r = call i32 @accumulate(i32 %a, i32 %b, i8 %n)
  ret i32 %r
}
)",
                      Context);
  ASSERT_TRUE(Module);
  auto *Function = Module->getFunction("accumulate");
  auto *Caller = Module->getFunction("caller");
  Function->setLinkage(llvm::GlobalValue::InternalLinkage);
  Function->addFnAttr(llvm::Attribute::NoUnwind);
  const auto Attributes = Function->getAttributes();
  const auto BeforeCaller = Caller->getInstructionCount();
  EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module, Function), 1U);
  EXPECT_EQ(Function, Module->getFunction("accumulate"));
  EXPECT_TRUE(Function->hasInternalLinkage());
  EXPECT_EQ(Attributes, Function->getAttributes());
  EXPECT_EQ(Function->getArg(0)->getName(), "seed");
  EXPECT_EQ(Caller->getInstructionCount(), BeforeCaller);
  auto *Call = llvm::dyn_cast<llvm::CallInst>(&Caller->front().front());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->getCalledFunction(), Function);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  const auto Source = emit(*Module);
  compileAndRun(Source + R"(
int main(void) {
  for (unsigned n = 0; n < 256; ++n)
    if (caller(0xfffffff0U, 29U, n) != 0xfffffff0U + 29U * (n & 3U)) return 1;
  return 0;
}
)",
                "-O0", {}, true);
}

TEST(LLVMCScalarLoopRecovery, RefusedBudgetsKeepCompleteBodiesAndDeclarations) {
  for (unsigned Kind = 0; Kind != 6; ++Kind) {
    llvm::LLVMContext Context;
    auto Module = parse(Prefix, Context);
    ASSERT_TRUE(Module);
    const auto Before = text(*Module);
    neverd::analysis::LLVMScalarLoopRecoveryLimits Limits;
    switch (Kind) {
    case 0:
      Limits.MaxConstructionWork = 0;
      break;
    case 1:
      Limits.MaxProofWork = 1;
      break;
    case 2:
      Limits.MaxCandidates = 0;
      break;
    case 3:
      Limits.MaxTransforms = 0;
      break;
    case 4:
      Limits.Proof.MaxWork = 0;
      break;
    case 5:
      Limits.MaxCandidates = 1;
      break;
    }
    EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module, nullptr, Limits), 0U);
    EXPECT_EQ(text(*Module), Before);
  }
}

TEST(LLVMCScalarLoopRecovery, MissingInputDefinednessDoesNotAuthorizeRecovery) {
  std::string IR = Prefix;
  IR.replace(IR.find("noundef"), 7, "");
  llvm::LLVMContext Context;
  auto Module = parse(IR, Context);
  ASSERT_TRUE(Module);
  const auto Before = text(*Module);
  EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module), 0U);
  EXPECT_EQ(text(*Module), Before);
  checkArithmetic(*Module, emit(*Module));
}

TEST(LLVMCScalarLoopRecovery, EffectfulCallsKeepTheirExecutionSites) {
  std::string IR = Prefix;
  IR.insert(IR.find("  %sum ="), "  call void @observe(i32 %value)\n");
  IR += "\ndeclare void @observe(i32)\n";
  llvm::LLVMContext Context;
  auto Module = parse(IR, Context);
  ASSERT_TRUE(Module);
  const auto Before = text(*Module);
  EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module), 0U);
  EXPECT_EQ(text(*Module), Before);
  const auto Source = emit(*Module);
  const std::string Main = R"(
static uint32_t calls, total;
void observe(uint32_t x) { ++calls; total += x; }
int main(void) {
  for (unsigned n = 0; n < 256; ++n) {
    calls = total = 0;
    if (accumulate(13U, 19U, n) != 13U + 19U * (n & 3U)) return 1;
    if (calls != ((n & 3U) ? (n & 3U) - 1U : 0U)) return 2;
    uint32_t expected = 0;
    for (unsigned i = 1; i < (n & 3U); ++i) expected += 13U + 19U * i;
    if (total != expected) return 3;
  }
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    compileAndRun(Source + Main, Level, {}, true);
}

TEST(LLVMCScalarLoopRecovery, FunctionAndExceptionMetadataRetainTheirBodies) {
  for (bool Table : {false, true}) {
    llvm::LLVMContext Context;
    auto Module = parse(Prefix, Context);
    ASSERT_TRUE(Module);
    if (Table)
      Module->getOrInsertNamedMetadata("neverd.windows.eh.functions");
    else
      Module->getFunction("accumulate")
          ->setMetadata("source.contract", llvm::MDNode::get(Context, {}));
    const auto Before = text(*Module);
    EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module), 0U);
    EXPECT_EQ(text(*Module), Before);
  }
}

TEST(LLVMCScalarLoopRecovery, ImageProjectionKeepsItsSeparateContract) {
  llvm::LLVMContext Context;
  auto Module = parse(Prefix, Context);
  ASSERT_TRUE(Module);
  neverd::BinaryImage Image;
  const auto Source = emit(*Module, nullptr, &Image);
  EXPECT_NE(Source.find("first"), std::string::npos) << Source;
  EXPECT_NE(Source.find("while (1)"), std::string::npos) << Source;
  checkArithmetic(*Module, Source);
}

std::string rotationFixture(bool Intrinsic) {
  std::string IR = Prefix;
  for (const auto &[Result, Operand] :
       {std::pair<std::string, std::string>{"first", "seed"},
        {"sum", "value"}}) {
    const auto Old = "  %" + Result + " = add i32 %" + Operand + ", %delta";
    const auto Replacement =
        Intrinsic ? "  %" + Result + " = call i32 @llvm.fshl.i32(i32 %" +
                        Operand + ", i32 %" + Operand + ", i32 7)"
                  : "  %" + Result + ".left = shl i32 %" + Operand +
                        ", 7\n  %" + Result + ".right = lshr i32 %" + Operand +
                        ", 25\n  %" + Result + " = or i32 %" + Result +
                        ".left, %" + Result + ".right";
    IR.replace(IR.find(Old), Old.size(), Replacement);
  }
  if (Intrinsic)
    IR += "\ndeclare i32 @llvm.fshl.i32(i32, i32, i32)\n";
  return IR;
}

TEST(LLVMCScalarLoopRecovery, ExistingAndNewIntrinsicDeclarationsRemainBound) {
  for (bool Intrinsic : {false, true}) {
    llvm::LLVMContext Context;
    auto Module = parse(rotationFixture(Intrinsic), Context);
    ASSERT_TRUE(Module);
    auto *Original = Module->getFunction("llvm.fshl.i32");
    EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module), 1U);
    auto *Helper = Module->getFunction("llvm.fshl.i32");
    ASSERT_TRUE(Helper);
    EXPECT_TRUE(Helper->isDeclaration());
    if (Original)
      EXPECT_EQ(Helper, Original);
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    const auto Source = emit(*Module);
    EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
    const std::string Main = R"(
int main(void) {
  uint32_t x = 1;
  for (unsigned sample = 0; sample < 512; ++sample) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    for (unsigned n = 0; n < 256; ++n) {
      uint32_t expected = x;
      for (unsigned i = 0; i < (n & 3U); ++i)
        expected = (expected << 7) | (expected >> 25);
      if (accumulate(x, ~x, n) != expected) return 1;
    }
  }
  return 0;
}
)";
    for (llvm::StringRef Level : {"-O0", "-O2"})
      compileAndRun(Source + Main, Level, {}, true);
  }
}

TEST(LLVMCScalarLoopRecovery,
     IntrinsicNameCollisionRefusesPublicationAtomically) {
  llvm::LLVMContext Context;
  auto Module = parse(
      rotationFixture(false) + "\n@llvm.fshl.i32 = global i32 17\n", Context);
  ASSERT_TRUE(Module);
  const auto Before = text(*Module);
  EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module), 0U);
  EXPECT_EQ(text(*Module), Before);
}

TEST(LLVMCScalarLoopRecovery, SharedTransformBudgetCannotRestartPerFunction) {
  llvm::LLVMContext Context;
  std::string Second = Prefix;
  Second.replace(Second.find("@accumulate"), 11, "@second");
  auto Module = parse(std::string(Prefix) + Second, Context);
  ASSERT_TRUE(Module);
  auto *SecondFunction = Module->getFunction("second");
  auto *First = Module->getFunction("accumulate");
  neverd::analysis::LLVMScalarLoopRecoveryLimits Limits;
  // This allows the first search but not its required continuation after
  // cleanup. Neither that partial result nor the next function may publish.
  Limits.MaxTransforms = 1;
  const auto Before = text(*Module);
  EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module, nullptr, Limits), 0U);
  EXPECT_EQ(text(*Module), Before);
  EXPECT_EQ(Module->getFunction("second"), SecondFunction);
  EXPECT_EQ(Module->getFunction("accumulate"), First);
}

TEST(LLVMCScalarLoopRecovery, ForeignFunctionSelectionCannotRewriteTheModule) {
  llvm::LLVMContext Context;
  auto Module = parse(Prefix, Context);
  auto Other = parse(Prefix, Context);
  ASSERT_TRUE(Module);
  ASSERT_TRUE(Other);
  const auto Before = text(*Module);
  EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module,
                                              Other->getFunction("accumulate")),
            0U);
  EXPECT_EQ(text(*Module), Before);
}

TEST(LLVMCScalarLoopRecovery, ExternalBlockAddressesRetainTheirTargetBodies) {
  llvm::LLVMContext Context;
  auto Module = parse(
      std::string(Prefix) +
          "\n@saved_label = global ptr blockaddress(@accumulate, %loop)\n",
      Context);
  ASSERT_TRUE(Module);
  const auto Before = text(*Module);
  EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module), 0U);
  EXPECT_EQ(text(*Module), Before);
}
} // namespace
