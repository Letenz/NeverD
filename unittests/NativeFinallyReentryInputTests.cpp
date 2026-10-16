//===- NativeFinallyReentryInputTests.cpp - MSVC chain observations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check source chain observations without granting native guard retirement.

#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Verifier.h"

#include <cstdlib>
#include <filesystem>

namespace neverd {
namespace {

TEST(PipelineOutcome, NativeFinallyReentryRetainsExactChainState) {
  const char *Root = std::getenv("NEVERD_NONLOCAL_FINALLY_INPUTS");
  if (!Root)
    GTEST_SKIP() << "set NEVERD_NONLOCAL_FINALLY_INPUTS to native MSVC images";
  const unsigned PreviousThreads = workerThreadOverride().load();
  setWorkerThreadCount(2);
  auto RestoreThreads = llvm::scope_exit(
      [PreviousThreads] { setWorkerThreadCount(PreviousThreads); });
  for (const char *Profile : {"Od-no-gs", "Od-gs", "O2-no-gs", "O2-gs"}) {
    const auto Path =
        std::filesystem::path(Root) / "x86" / Profile / "original.exe";
    auto Image = loadBinary(Path.string());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    const auto *Export = Image->findExport("nonlocal_finally_reenter");
    ASSERT_NE(Export, nullptr);
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Profile << ':' << NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.SourceProjection = Options.LiftMode = true;
      Options.OnlyFunctionEntries.insert(Export->Addr);
      Options.NoOpt = NoOpt;
      const auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_NE(Result.LlvmModule, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      unsigned Checked = 0;
      for (const auto &Function : Result.LowFuncs) {
        if (Function.Entry != Export->Addr)
          continue;
        ASSERT_TRUE(Function.RegistrationStates);
        const auto &States = *Function.RegistrationStates;
        EXPECT_TRUE(States.Complete);
        EXPECT_TRUE(States.CallbackStatesComplete);
        EXPECT_TRUE(States.RegistrationLifetimeComplete);
        EXPECT_TRUE(States.SEHInvocationExitsComplete);
        EXPECT_TRUE(States.ChainOperationsComplete);
        EXPECT_TRUE(States.IncomingFrameAccessesComplete);
        // Exact chain equality does not prove that an abnormal parent jump
        // retires the CRT guard. Keep that independent native obligation.
        EXPECT_FALSE(States.SEHParentChainComplete);
        ++Checked;
      }
      ASSERT_EQ(Checked, 1u);
      auto Native = exception_rewrite::validateExceptionRewriteContracts(
          *Result.LlvmModule);
      EXPECT_FALSE(bool(Native));
      if (!Native)
        llvm::consumeError(Native.takeError());
    }
  }
}

} // namespace
} // namespace neverd
