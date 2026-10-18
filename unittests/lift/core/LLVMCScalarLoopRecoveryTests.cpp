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

/// Where \p Source defines the function whose header starts with \p Header:
/// the header's line that opens its body.  A whole module declares each
/// definition before the bodies (DefinitionDeclarations), which a unit of
/// one function leaves out.
size_t definitionAt(llvm::StringRef Source, llvm::StringRef Header) {
  for (size_t At = Source.find(Header); At != llvm::StringRef::npos;
       At = Source.find(Header, At + 1)) {
    const size_t End = Source.find('\n', At);
    if (Source.slice(At, End).rtrim().ends_with("{"))
      return At;
  }
  return llvm::StringRef::npos;
}

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

void checkArithmetic(llvm::Module &Module, const std::string &Source,
                     llvm::StringRef CountType = "uint8_t") {
  Module.getFunction("accumulate")->setName("reference_accumulate");
  const std::string Reference = text(Module);
  Module.getFunction("reference_accumulate")->setName("accumulate");
  const std::string Main =
      "\nextern uint32_t reference_accumulate(uint32_t, uint32_t, " +
      CountType.str() + ");\n" + R"(
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

std::string encodedStopFixture(unsigned Width = 8) {
  std::string IR = Prefix;
  const std::string Stop = "  %done = icmp eq i8 %next, %bound";
  IR.replace(IR.find(Stop), Stop.size(), R"(  %complement = xor i8 %i, -1
  %remaining = add i8 %bound, %complement
  %biased = add i8 %remaining, 127
  %encoded = or i8 %biased, %remaining
  %done = icmp sge i8 %encoded, 0)");
  if (Width != 8) {
    const auto Type = "i" + std::to_string(Width);
    size_t Position = 0;
    while ((Position = IR.find("i8 ", Position)) != std::string::npos) {
      IR.replace(Position, 2, Type);
      Position += Type.size();
    }
    IR.replace(IR.find(", 127"), 5,
               ", " + std::to_string((uint64_t(1) << (Width - 1)) - 1));
  }
  return IR;
}

TEST(LLVMCScalarLoopRecovery, NewlyExposedModularStopsComposeWithLoopRecovery) {
  // The encoded sign test is true exactly when next == bound. Recovery
  // first exposes it; predicate cleanup then needs invariant motion and a
  // shared SSA identity for the two equivalent counter updates.
  for (unsigned Width : {8U, 32U, 64U}) {
    SCOPED_TRACE(Width);
    llvm::LLVMContext Context;
    auto Module = parse(encodedStopFixture(Width), Context);
    ASSERT_TRUE(Module);
    const auto Source = emit(*Module);
    EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
    EXPECT_EQ(Source.find("while (1)"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
    EXPECT_EQ(Source.find("127"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("0x7F"), std::string::npos) << Source;
    checkArithmetic(*Module, Source, "uint" + std::to_string(Width) + "_t");
  }
}

TEST(LLVMCScalarLoopRecovery, ModularStopDoesNotHideSourceOverflow) {
  auto IR = encodedStopFixture();
  const std::string Before = "%biased = add i8";
  // A positive remaining value plus the bias overflows signed addition on a
  // reachable iteration. Algebraic truth of the condition cannot erase poison.
  IR.replace(IR.find(Before), Before.size(), "%biased = add nsw i8");
  llvm::LLVMContext Context;
  auto Module = parse(IR, Context);
  ASSERT_TRUE(Module);
  const auto Original = text(*Module);
  EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module), 0U);
  EXPECT_EQ(text(*Module), Original);
}

TEST(LLVMCScalarLoopRecovery, PredicateConstructionBudgetIsSharedAndAtomic) {
  const auto IR = encodedStopFixture();
  auto Attempt = [&](uint64_t Work) {
    llvm::LLVMContext Context;
    auto Module = parse(IR, Context);
    if (!Module)
      return false;
    const auto Before = text(*Module);
    neverd::analysis::LLVMScalarSourceRecoveryLimits Limits;
    Limits.Search.MaxConstructionWork = Work;
    const auto Changed =
        neverd::llvmc::recoverScalarLoops(*Module, nullptr, Limits);
    if (!Changed)
      EXPECT_EQ(text(*Module), Before);
    return Changed == 1;
  };
  // Find the publication boundary without fixing an implementation work
  // count. Every unsuccessful run must preserve the complete original body.
  uint64_t Low = 0;
  uint64_t High = neverd::analysis::LLVMScalarSourceRecoveryLimits()
                      .Search.MaxConstructionWork;
  ASSERT_TRUE(Attempt(High));
  while (High - Low > 1) {
    const auto Middle = Low + (High - Low) / 2;
    if (Attempt(Middle))
      High = Middle;
    else
      Low = Middle;
  }
  EXPECT_FALSE(Attempt(High - 1));
  EXPECT_TRUE(Attempt(High));

  auto Second = IR;
  Second.replace(Second.find("@accumulate"), 11, "@second");
  llvm::LLVMContext Context;
  auto Module = parse(IR + Second, Context);
  ASSERT_TRUE(Module);
  auto *Untouched = Module->getFunction("second");
  std::string Before;
  llvm::raw_string_ostream Stream(Before);
  Untouched->print(Stream);
  neverd::analysis::LLVMScalarSourceRecoveryLimits Limits;
  Limits.Search.MaxConstructionWork = High;
  EXPECT_EQ(neverd::llvmc::recoverScalarLoops(*Module, nullptr, Limits), 1U);
  std::string After;
  llvm::raw_string_ostream AfterStream(After);
  Untouched->print(AfterStream);
  EXPECT_EQ(After, Before);
}

TEST(LLVMCScalarLoopRecovery, InternalWidthDropsRedundantWideContainers) {
  constexpr char IR[] = R"(
define i32 @accumulate(i32 noundef %seed, i32 noundef %delta, i8 noundef %count) {
entry:
  %bound = and i8 %count, 3
  %wide.seed = zext i32 %seed to i64
  %wide.delta = zext i32 %delta to i64
  br label %head
head:
  %i = phi i8 [0, %entry], [%next, %body]
  %value = phi i64 [%wide.seed, %entry], [%sum, %body]
  %more = icmp ult i8 %i, %bound
  br i1 %more, label %body, label %exit
body:
  %sum = add i64 %value, %wide.delta
  %next = add nuw i8 %i, 1
  br label %head
exit:
  %result = trunc i64 %value to i32
  ret i32 %result
})";
  llvm::LLVMContext Context;
  auto Module = parse(IR, Context);
  ASSERT_TRUE(Module);
  const auto Source = emit(*Module);
  EXPECT_EQ(Source.find("uint64_t"), std::string::npos) << Source;
  EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
  checkArithmetic(*Module, Source);
}

TEST(LLVMCScalarLoopRecovery, WidthAndCarrierMasksComposeBeforeEmission) {
  constexpr char IR[] = R"(
define i32 @accumulate(i32 noundef %seed, i32 noundef %delta, i8 noundef %count) {
entry:
  %bound = and i8 %count, 3
  %wide.seed = zext i32 %seed to i64
  %wide.delta = zext i32 %delta to i64
  br label %head
head:
  %i = phi i8 [0, %entry], [%next, %body]
  %value = phi i64 [%wide.seed, %entry], [%sum, %body]
  %masked = and i8 %i, 15
  %more = icmp ult i8 %masked, %bound
  br i1 %more, label %body, label %exit
body:
  %sum = add i64 %value, %wide.delta
  %next = add nuw i8 %masked, 1
  br label %head
exit:
  %result = trunc i64 %value to i32
  ret i32 %result
})";
  llvm::LLVMContext Context;
  auto Module = parse(IR, Context);
  ASSERT_TRUE(Module);
  const auto Source = emit(*Module);
  EXPECT_EQ(Source.find("uint64_t"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("& 0xF"), std::string::npos) << Source;
  EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  checkArithmetic(*Module, Source);
}

TEST(LLVMCScalarLoopRecovery, WidenedLastIndexKeepsTheZeroTripCase) {
  // A separate body and latch, with a narrow wrapped last index. Reusing that
  // index plus one as the only candidate bound would execute 256 extra steps
  // for an empty loop. The variable delta makes every extra step observable.
  constexpr char IR[] = R"(
define i32 @accumulate(i32 noundef %seed, i32 noundef %delta, i8 noundef %count) {
entry:
  %bound = and i8 %count, 3
  %empty = icmp eq i8 %bound, 0
  br i1 %empty, label %exit, label %pre
pre:
  %last.byte = sub i8 %bound, 1
  %last = zext i8 %last.byte to i32
  br label %head
head:
  %i = phi i32 [0, %pre], [%next, %latch]
  %value = phi i32 [%seed, %pre], [%sum, %latch]
  br label %body
body:
  %sum = add i32 %value, %delta
  br label %latch
latch:
  %next = add nuw i32 %i, 1
  %done = icmp eq i32 %i, %last
  br i1 %done, label %exit, label %head
exit:
  %result = phi i32 [%seed, %entry], [%sum, %latch]
  ret i32 %result
})";
  llvm::LLVMContext Context;
  auto Module = parse(IR, Context);
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
    neverd::analysis::LLVMScalarSourceRecoveryLimits Limits;
    switch (Kind) {
    case 0:
      Limits.Search.MaxConstructionWork = 0;
      break;
    case 1:
      Limits.Search.MaxProofWork = 1;
      break;
    case 2:
      Limits.Search.MaxCandidates = 0;
      break;
    case 3:
      Limits.Search.MaxTransforms = 0;
      break;
    case 4:
      Limits.Search.Proof.MaxWork = 0;
      break;
    case 5:
      Limits.Search.MaxCandidates = 1;
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

TEST(LLVMCScalarLoopRecovery, MemoryRegionsPreserveAliasCallsAndPointerPhis) {
  const char *IR = R"(
define i64 @memory_loop(ptr %a, ptr %b, i32 %limit, ptr %observer) {
entry:
  br label %header
header:
  %i = phi i32 [0, %entry], [%next, %latch]
  %p = phi ptr [%a, %entry], [%q, %latch]
  %q = phi ptr [%b, %entry], [%p, %latch]
  %sum = phi i64 [0, %entry], [%total, %latch]
  %more = icmp ult i32 %i, %limit
  br i1 %more, label %body, label %exit
body:
  %before = load i64, ptr %p, align 1
  %low = and i32 %i, 1
  %odd = icmp ne i32 %low, 0
  br i1 %odd, label %left, label %right
left:
  %inc = add i64 %before, 17
  store i64 %inc, ptr %q, align 1
  br label %latch
right:
  %flip = xor i64 %before, 123456789
  store i64 %flip, ptr %p, align 1
  br label %latch
latch:
  %observed = call i64 %observer(ptr %q, i32 %i)
  %after = load i64, ptr %p, align 1
  %part = xor i64 %after, %observed
  %total = add i64 %sum, %part
  %next = add i32 %i, 1
  br label %header
exit:
  ret i64 %sum
}
define i64 @memory_diamond(ptr %p, i32 %which, ptr %observer) {
entry:
  %old = load i64, ptr %p, align 1
  %test = icmp eq i32 %which, 0
  br i1 %test, label %a, label %b
a:
  store i64 23, ptr %p, align 1
  %x = call i64 %observer(ptr %p, i32 1)
  br label %done
b:
  %y = call i64 %observer(ptr %p, i32 2)
  store i64 %old, ptr %p, align 1
  br label %done
done:
  %result = phi i64 [%x, %a], [%y, %b]
  %last = load i64, ptr %p, align 1
  %answer = xor i64 %result, %last
  ret i64 %answer
}
define i64 @guarded_diamond(ptr %p, i32 %which, ptr %observer) {
entry:
  %test = icmp eq i32 %which, 0
  br i1 %test, label %a, label %b
a:
  %x = call i64 %observer(ptr %p, i32 4)
  br label %continue
b:
  %y = call i64 %observer(ptr %p, i32 3)
  %low = and i64 %y, 1
  %bad = icmp ne i64 %low, 0
  br i1 %bad, label %return, label %continue
continue:
  %chosen = phi i64 [%x, %a], [%y, %b]
  %last = load i64, ptr %p, align 1
  %updated = add i64 %last, %chosen
  store i64 %updated, ptr %p, align 1
  %observed = call i64 %observer(ptr %p, i32 5)
  br label %return
return:
  %result = phi i64 [%y, %b], [%observed, %continue]
  ret i64 %result
}
)";
  llvm::LLVMContext Context;
  auto Module = parse(IR, Context);
  ASSERT_TRUE(Module);
  const auto Source = emit(*Module);
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
  Module->getFunction("memory_loop")->setName("reference_loop");
  Module->getFunction("memory_diamond")->setName("reference_diamond");
  Module->getFunction("guarded_diamond")->setName("reference_guarded");
  const auto Reference = text(*Module);
  const std::string Main = R"(
#include <string.h>
extern uint64_t reference_loop(void *, void *, uint32_t, void *);
extern uint64_t reference_diamond(void *, uint32_t, void *);
extern uint64_t reference_guarded(void *, uint32_t, void *);
static uint64_t trace[64];
static unsigned count;
static uint64_t observe(void *p, uint32_t tag) {
  uint64_t value;
  memcpy(&value, p, 8);
  trace[count++] = value ^ ((uint64_t)tag << 40);
  value = value * 17U + tag;
  memcpy(p, &value, 8);
  return value ^ 0x8000000000000041ULL;
}
int main(void) {
  for (unsigned seed = 0; seed < 2048; ++seed) {
    uint64_t a[8], b[8], expected[64];
    for (unsigned i = 0; i < 8; ++i)
      a[i] = b[i] = ((uint64_t)seed << (i + 19)) ^ (i * 0x87654321U);
    unsigned j = (seed >> 5) & 3, n = seed & 31;
    count = 0;
    uint64_t want = reference_loop(a, a + j, n, (void *)observe);
    unsigned calls = count;
    memcpy(expected, trace, calls * 8);
    count = 0;
    if (memory_loop(b, b + j, n, (void *)observe) != want || count != calls ||
        memcmp(a, b, sizeof(a)) || memcmp(expected, trace, calls * 8)) return 1;
    count = 0;
    want = reference_diamond(a, seed & 1, (void *)observe);
    calls = count;
    memcpy(expected, trace, calls * 8);
    count = 0;
    if (memory_diamond(b, seed & 1, (void *)observe) != want || count != calls ||
        memcmp(a, b, sizeof(a)) || memcmp(expected, trace, calls * 8)) return 2;
    count = 0;
    want = reference_guarded(a, seed & 1, (void *)observe);
    calls = count;
    memcpy(expected, trace, calls * 8);
    count = 0;
    if (guarded_diamond(b, seed & 1, (void *)observe) != want || count != calls ||
        memcmp(a, b, sizeof(a)) || memcmp(expected, trace, calls * 8)) return 3;
  }
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    compileAndRun(Source + Main, Level, Reference, true);
}

TEST(LLVMCScalarLoopRecovery, MemoryLoopFailureExitsKeepTheirPhiResults) {
  llvm::LLVMContext Context;
  auto Module = parse(R"(
define i64 @guarded_loop(ptr %p, i32 %limit, i32 %fail) {
entry:
  %empty = icmp eq i32 %limit, 0
  br i1 %empty, label %return, label %pre
pre:
  br label %header
header:
  %i = phi i32 [0, %pre], [%next, %step]
  %more = icmp ult i32 %i, %limit
  br i1 %more, label %body, label %done
body:
  %value = load i64, ptr %p, align 1
  %sum = add i64 %value, 13
  store i64 %sum, ptr %p, align 1
  %bad = icmp eq i32 %i, %fail
  br i1 %bad, label %return, label %step
step:
  %next = add i32 %i, 1
  br label %header
done:
  %last = load i64, ptr %p, align 1
  %answer = xor i64 %last, 12345
  br label %return
return:
  %result = phi i64 [17, %entry], [%sum, %body], [%answer, %done]
  ret i64 %result
}
)",
                      Context);
  ASSERT_TRUE(Module);
  const auto Source = emit(*Module);
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  Module->getFunction("guarded_loop")->setName("reference_loop");
  const auto Reference = text(*Module);
  const std::string Main = R"(
extern uint64_t reference_loop(void *, uint32_t, uint32_t);
int main(void) {
  for (unsigned n=0;n<32;++n) for(unsigned fail=0;fail<33;++fail) {
    uint64_t a=0xfedcba9876543210ULL, b=a;
    uint64_t x=reference_loop(&a,n,fail), y=guarded_loop(&b,n,fail);
    if(x!=y || a!=b) return 1;
  }
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    compileAndRun(Source + Main, Level, Reference, true);
}

TEST(LLVMCScalarLoopRecovery,
     EffectfulGuardLaddersKeepSharedFailureAndEdgeValues) {
  llvm::LLVMContext Context;
  auto Module = parse(R"(
declare i64 @guard11(ptr, i64)
define i64 @ladder(ptr %a, ptr %b, i32 %mode, ptr %observe) {
entry:
  %low = and i32 %mode, 1
  %enabled = icmp ne i32 %low, 0
  br i1 %enabled, label %test1, label %failure
test1:
  %before = load i64, ptr %b, align 1
  %one = call i64 @guard11(ptr %a, i64 %before)
  %bit = and i32 %mode, 2
  %valid = icmp eq i32 %bit, 0
  br i1 %valid, label %test2, label %failure
test2:
  %after = load i64, ptr %a, align 1
  %two = call i64 %observe(ptr %b, i64 %after)
  %bit2 = and i32 %mode, 4
  %bad = icmp ne i32 %bit2, 0
  br i1 %bad, label %failure, label %success
success:
  %combined = xor i64 %one, %two
  store i64 %combined, ptr %b, align 1
  br label %join
failure:
  %reason = phi i64 [13, %entry], [%one, %test1], [%two, %test2]
  %pointer = phi ptr [%a, %entry], [%a, %test1], [%b, %test2]
  %fallback = call i64 %observe(ptr %pointer, i64 %reason)
  %halt = and i32 %mode, 8
  %stop = icmp ne i32 %halt, 0
  br i1 %stop, label %return, label %join
join:
  %chosen = phi i64 [%combined, %success], [%fallback, %failure]
  %old = load i64, ptr %a, align 1
  %answer = add i64 %old, %chosen
  store i64 %answer, ptr %a, align 1
  br label %return
return:
  %result = phi i64 [%fallback, %failure], [%answer, %join]
  ret i64 %result
}
)",
                      Context);
  ASSERT_TRUE(Module);
  const auto Source = emit(*Module);
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  EXPECT_NE(Source.find("guard"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("uint8_t guard11 ="), std::string::npos) << Source;
  const auto Selected = emit(*Module, Module->getFunction("ladder"));
  EXPECT_EQ(Selected.substr(definitionAt(Selected, "uint64_t ladder(")),
            Source.substr(definitionAt(Source, "uint64_t ladder(")));
  Module->getFunction("ladder")->setName("reference_ladder");
  const auto Reference = text(*Module);
  const std::string Main = R"(
#include <string.h>
extern uint64_t reference_ladder(void *, void *, uint32_t, void *);
static uint64_t trace[32];
static unsigned count;
static uint64_t observe(void *p, uint64_t tag) {
  uint64_t old;
  memcpy(&old, p, 8);
  trace[count++] = old;
  trace[count++] = tag;
  uint64_t value = (old * 17U) ^ tag;
  memcpy(p, &value, 8);
  return value + 0x8000000000000001ULL;
}
uint64_t guard11(void *p, uint64_t tag) { return observe(p, tag); }
int main(void) {
  for (unsigned seed = 0; seed < 2048; ++seed) {
    uint64_t a[4], b[4], saved[32];
    for (unsigned i=0;i<4;++i) a[i]=b[i]=((uint64_t)seed<<(i+19))^(i*0x7654321U);
    unsigned mode=seed & 15, offset=(seed>>4)&3;
    void *ap=(mode&1) ? a+offset : 0, *bp=(mode&1) ? b+offset : 0;
    count=0;
    uint64_t want=reference_ladder(a,ap,mode,(void *)observe);
    unsigned calls=count;
    memcpy(saved,trace,calls*8);
    count=0;
    if(ladder(b,bp,mode,(void *)observe)!=want || count!=calls ||
       memcmp(a,b,sizeof a) || memcmp(saved,trace,calls*8)) return 1;
  }
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    compileAndRun(Source + Main, Level, Reference, true);
}

TEST(LLVMCScalarLoopRecovery, NestedOptionalCallsKeepTheirOuterContinuation) {
  llvm::LLVMContext Context;
  auto Module = parse(R"(
define i64 @optional_calls(ptr %p, i32 %mode, ptr %observe) {
entry:
  %enabled = icmp ne i32 %mode, 0
  br i1 %enabled, label %start, label %finish
start:
  %first = call i64 %observe(ptr %p, i64 11)
  %bad1 = icmp eq i64 %first, 7
  br i1 %bad1, label %return1, label %body
return1:
  ret i64 %first
body:
  %bit = and i32 %mode, 1
  %visit = icmp ne i32 %bit, 0
  br i1 %visit, label %optional, label %middle
optional:
  %second = call i64 %observe(ptr %p, i64 22)
  %bad2 = icmp eq i64 %second, 7
  br i1 %bad2, label %return2, label %middle
return2:
  ret i64 %second
middle:
  %value = load i64, ptr %p, align 1
  %bit2 = and i32 %mode, 2
  %visit2 = icmp ne i32 %bit2, 0
  br i1 %visit2, label %again, label %finish
again:
  %third = call i64 %observe(ptr %p, i64 33)
  %bad3 = icmp eq i64 %third, 7
  br i1 %bad3, label %return3, label %after
return3:
  ret i64 %third
after:
  %updated = load i64, ptr %p, align 1
  br label %finish
finish:
  %chosen = phi i64 [0, %entry], [%value, %middle], [%updated, %after]
  %answer = add i64 %chosen, 1
  store i64 %answer, ptr %p, align 1
  ret i64 %answer
}
)",
                      Context);
  ASSERT_TRUE(Module);
  const auto Source = emit(*Module);
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  Module->getFunction("optional_calls")->setName("reference_calls");
  const auto Reference = text(*Module);
  const std::string Main = R"(
#include <string.h>
extern uint64_t reference_calls(void *, uint32_t, void *);
static uint64_t trace[8];
static unsigned count;
static uint64_t observe(void *p, uint64_t tag) {
  uint64_t value;
  memcpy(&value,p,8);
  value=(value*17U)^tag;
  memcpy(p,&value,8);
  trace[count++]=value;
  return value&15;
}
int main(void) {
  for(unsigned seed=0;seed<2048;++seed) {
    uint64_t a=seed*0x123456789ULL, b=a, saved[8];
    count=0;
    uint64_t want=reference_calls(&a,seed&7,(void *)observe);
    unsigned calls=count;
    memcpy(saved,trace,calls*8);
    count=0;
    if(optional_calls(&b,seed&7,(void *)observe)!=want || a!=b || count!=calls ||
       memcmp(saved,trace,calls*8)) return 1;
  }
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    compileAndRun(Source + Main, Level, Reference, true);
}

TEST(LLVMCScalarLoopRecovery, DistinctLoopExitTailsKeepEffectsAndCommonPhis) {
  llvm::LLVMContext Context;
  auto Module = parse(R"(
define i64 @exit_tails(ptr %p, ptr %q, i32 %n, i32 %fail, i32 %mode, ptr %observe) {
entry:
  br label %header
header:
  %i = phi i32 [0, %entry], [%next, %step]
  %left = phi ptr [%p, %entry], [%right, %step]
  %right = phi ptr [%q, %entry], [%left, %step]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %body, label %finished
body:
  %old = load i64, ptr %left, align 1
  %value = add i64 %old, 17
  store i64 %value, ptr %right, align 1
  %bad = icmp eq i32 %i, %fail
  br i1 %bad, label %cancelled, label %check
check:
  %seen = call i64 %observe(ptr %right, i64 %value)
  %again = icmp eq i32 %i, %mode
  br i1 %again, label %cancelled, label %check_escape
check_escape:
  %escape_index = add i32 %mode, 3
  %other = icmp eq i32 %i, %escape_index
  br i1 %other, label %escaped, label %step
step:
  %observed = call i64 %observe(ptr %left, i64 %value)
  %next = add i32 %i, 1
  br label %header
finished:
  %final = call i64 %observe(ptr %left, i64 19)
  br label %join
cancelled:
  %why = phi i64 [%value, %body], [%seen, %check]
  store i64 23, ptr %left, align 1
  %cancel = call i64 %observe(ptr %right, i64 %why)
  br label %join
escaped:
  %escape = call i64 %observe(ptr %left, i64 31)
  store i64 %escape, ptr %right, align 1
  br label %join
join:
  %a = phi ptr [%left, %finished], [%right, %cancelled], [%left, %escaped]
  %b = phi ptr [%right, %finished], [%left, %cancelled], [%right, %escaped]
  %v = phi i64 [%final, %finished], [%cancel, %cancelled], [%escape, %escaped]
  %x = load i64, ptr %a, align 1
  %y = load i64, ptr %b, align 1
  %xy = xor i64 %x, %y
  %result = add i64 %xy, %v
  store i64 %result, ptr %a, align 1
  ret i64 %result
}
)",
                      Context);
  ASSERT_TRUE(Module);
  const auto Source = emit(*Module);
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  EXPECT_NE(Source.find("break;"), std::string::npos) << Source;
  const auto Selected = emit(*Module, Module->getFunction("exit_tails"));
  EXPECT_EQ(Selected.substr(definitionAt(Selected, "uint64_t exit_tails(")),
            Source.substr(definitionAt(Source, "uint64_t exit_tails(")));
  Module->getFunction("exit_tails")->setName("reference_exits");
  const auto Reference = text(*Module);
  const std::string Main = R"(
#include <string.h>
extern uint64_t reference_exits(void *, void *, uint32_t, uint32_t, uint32_t, void *);
static uint64_t trace[128];
static unsigned count;
static uint64_t observe(void *p, uint64_t tag) {
  uint64_t old;
  memcpy(&old,p,8);
  trace[count++]=old;
  trace[count++]=tag;
  old=(old*13U)+tag;
  memcpy(p,&old,8);
  return old^0x8000000000000000ULL;
}
int main(void) {
  for(unsigned seed=0;seed<2048;++seed) {
    uint64_t a[4],b[4],saved[128];
    for(unsigned i=0;i<4;++i) a[i]=b[i]=((uint64_t)seed<<(i+13))^(i*0xfedcba9U);
    unsigned n=seed&15, fail=(seed>>4)&31, mode=(seed>>8)&7, offset=(seed>>6)&3;
    count=0;
    uint64_t want=reference_exits(a,a+offset,n,fail,mode,(void *)observe);
    unsigned calls=count;
    memcpy(saved,trace,calls*8);
    count=0;
    if(exit_tails(b,b+offset,n,fail,mode,(void *)observe)!=want || count!=calls ||
       memcmp(a,b,sizeof a) || memcmp(saved,trace,calls*8)) return 1;
  }
  return 0;
}
)";
  for (llvm::StringRef Level : {"-O0", "-O2"})
    compileAndRun(Source + Main, Level, Reference, true);
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
  neverd::analysis::LLVMScalarSourceRecoveryLimits Limits;
  // This allows the first search but not its required continuation after
  // cleanup. Neither that partial result nor the next function may publish.
  Limits.Search.MaxTransforms = 1;
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
