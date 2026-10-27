//===- NativeFinallyInputTests.cpp - Native MSVC source projection --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks source projection of explicitly supplied native MSVC finally inputs.

#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/LLVMCallContract.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Support/FileSystem.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace neverd {
namespace {

TEST(PipelineOutcome, NativeFinallyInputsRetainSourceLLVM) {
  const char *Root = std::getenv("NEVERD_NONLOCAL_FINALLY_INPUTS");
  if (!Root)
    GTEST_SKIP() << "set NEVERD_NONLOCAL_FINALLY_INPUTS to native MSVC images";
  const unsigned PreviousThreads = workerThreadOverride().load();
  setWorkerThreadCount(2);
  auto RestoreThreads = llvm::scope_exit(
      [PreviousThreads] { setWorkerThreadCount(PreviousThreads); });
  for (const char *Architecture : {"x86", "x64"})
    for (const char *Profile : {"Od-no-gs", "Od-gs", "O2-no-gs", "O2-gs"}) {
      const auto Path =
          std::filesystem::path(Root) / Architecture / Profile / "original.exe";
      auto Image = loadBinary(Path.string());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      ASSERT_EQ(Image->Format, BinaryFormat::COFF);
      ASSERT_EQ(Image->Arch,
                std::string(Architecture) == "x86" ? Arch::X86 : Arch::X64);
      for (const char *Name :
           {"nonlocal_finally_goto", "nonlocal_finally_return"}) {
        const auto *Export = Image->findExport(Name);
        ASSERT_NE(Export, nullptr) << Path << ':' << Name;
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << Path << ':' << Name << ':' << NoOpt);
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
          EXPECT_TRUE(validateResolvedLLVMCallSignatures(*Result.LlvmModule));
          ASSERT_FALSE(Result.LowFuncs.empty());
          for (const auto &Function : Result.LowFuncs)
            EXPECT_TRUE(Function.hasCompleteLiftCoverage()) << Function.Name;
          if (Image->Arch != Arch::X86)
            continue;
          auto Native = exception_rewrite::validateExceptionRewriteContracts(
              *Result.LlvmModule);
          bool Reconstruct = false;
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
          Reconstruct = true;
#endif
          if (Reconstruct) {
            ASSERT_TRUE(bool(Native)) << llvm::toString(Native.takeError());
            const auto *Source =
                Image->ExceptionMetadata.findFunction(Export->Addr);
            ASSERT_NE(Source, nullptr);
            unsigned Checked = 0;
            for (const auto &Function : *Result.LlvmModule) {
              auto Identity = rewrite_source::getOriginalVA(Function);
              ASSERT_TRUE(bool(Identity))
                  << llvm::toString(Identity.takeError());
              if (Function.isDeclaration() || *Identity != Export->Addr)
                continue;
              EXPECT_EQ(Function.arg_size(),
                        std::string(Name) == "nonlocal_finally_goto" ? 2u : 1u);
              auto Error =
                  validateCOFFRegistrationIR(Function, *Source, *Image);
              ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
              ++Checked;
            }
            ASSERT_EQ(Checked, 1u);
            if (const char *Output =
                    std::getenv("NEVERD_NONLOCAL_FINALLY_OUTPUT")) {
              std::error_code EC = llvm::sys::fs::create_directories(Output);
              ASSERT_FALSE(EC) << EC.message();
              const std::string Prefix =
                  std::string(Name) == "nonlocal_finally_goto"
                      ? "goto-" + std::string(Profile) + "-"
                  : std::string(Profile) == "Od-no-gs" ? "return-"
                  : std::string(Profile) == "Od-gs"
                      ? "return-gs-"
                      : "return-" + std::string(Profile) + "-";
              const auto IR = std::filesystem::path(Output) /
                              (Prefix + (NoOpt ? "raw.ll" : "opt.ll"));
              llvm::raw_fd_ostream OS(IR.string(), EC);
              ASSERT_FALSE(EC) << EC.message();
              Result.LlvmModule->print(OS, nullptr);
            }
          } else {
            // Native source identity requires the matching LLVM frame support.
            EXPECT_FALSE(bool(Native));
          }
          if (!Native)
            llvm::consumeError(Native.takeError());
          if (std::string(Profile) != "Od-gs" &&
              std::string(Profile) != "O2-gs")
            continue;
          const auto *EH = Image->ExceptionMetadata.findFunction(Export->Addr);
          ASSERT_NE(EH, nullptr);
          ASSERT_TRUE(EH->Registration);
          bool HasRetainedHandler = false;
          for (const auto &Function : *Result.LlvmModule) {
            auto Identity = rewrite_source::getOriginalVA(Function);
            ASSERT_TRUE(bool(Identity)) << llvm::toString(Identity.takeError());
            if (*Identity == EH->Registration->HandlerVA &&
                !Function.use_empty()) {
              EXPECT_TRUE(Function.isDeclaration());
              HasRetainedHandler = true;
            }
          }
          EXPECT_TRUE(HasRetainedHandler);
        }
      }
    }
}

} // namespace
} // namespace neverd
