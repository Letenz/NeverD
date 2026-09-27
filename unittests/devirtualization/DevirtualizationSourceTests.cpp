//===- DevirtualizationSourceTests.cpp - Public VM execution checks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <functional>
#include <map>
#include <tuple>

namespace {

using namespace neverd;

std::string readSource(const fs::path &Path) {
  std::ifstream Input(Path);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

va_t functionEntry(const BinaryImage &Image, const std::string &Name) {
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.Name == Name && Symbol.IsFunc)
      return Symbol.Addr;
  return InvalidVA;
}

bool hasCycle(const LowFunc &Function) {
  std::map<int, const LowBlock *> Blocks;
  std::map<int, unsigned> Color;
  for (const auto &Block : Function.Blocks)
    Blocks[Block.Id] = &Block;
  std::function<bool(int)> Visit = [&](int Id) {
    if (Color[Id] == 1)
      return true;
    if (Color[Id] == 2)
      return false;
    Color[Id] = 1;
    const auto Found = Blocks.find(Id);
    if (Found != Blocks.end())
      for (int Succ : Found->second->Succs)
        if (Visit(Succ))
          return true;
    Color[Id] = 2;
    return false;
  };
  for (const auto &Block : Function.Blocks)
    if (Visit(Block.Id))
      return true;
  return false;
}

analysis::SpecializationOptions recoveryOptions() {
  analysis::SpecializationOptions Options;
  Options.ControlRegisters.push_back({x86reg::R10, 8});
  return Options;
}

class DevirtualizationSourceTest : public NeverDLiftTest {
protected:
  static fs::path fixture(const char *Name) {
    return fs::path(TEST_SOURCE_DIR) / "fixtures" / Name;
  }

  RunResult buildFixture(const fs::path &Output, const char *Extra = nullptr) {
    std::vector<std::string> Args{"-target",
                                  "x86_64-linux-gnu",
                                  "-fuse-ld=lld",
                                  "-nostdlib",
                                  "-static",
                                  "-Wl,-e,generic_vm_register_arithmetic",
                                  fixture("generic_vm_register.S").string(),
                                  fixture("generic_vm_stack.S").string()};
    if (Extra)
      Args.push_back(fixture(Extra).string());
    Args.insert(Args.end(), {"-o", Output.string()});
    return exec(NEVERD_TEST_CLANG, Args);
  }
};

using SourceCase = std::tuple<const char *, unsigned, bool>;

class DevirtualizationRoundTripTest
    : public DevirtualizationSourceTest,
      public ::testing::WithParamInterface<SourceCase> {};

TEST_P(DevirtualizationRoundTripTest, RecoversMachineAndPreservesExecution) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM source recovery requires clang";
  const auto &[Name, Kind, LLVM] = GetParam();
  SCOPED_TRACE(Name);
  SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, Name);
  ASSERT_NE(Entry, InvalidVA);

  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.InterpreterSpecialization = recoveryOptions();
  Options.OnlyFunctionEntries.insert(Entry);
  Options.LiftMode = LLVM;
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.MedIRVerifierFailures, 0u);
  ASSERT_FALSE(Result.LLVMVerifierFailed);
  ASSERT_EQ(Result.BackendUnhandledValueIntrinsics, 0u);
  ASSERT_TRUE(Result.InterpreterRecovery.has_value());
  const auto &Recovery = *Result.InterpreterRecovery;
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  EXPECT_FALSE(Recovery.Reads.empty());
  EXPECT_FALSE(Recovery.Origins.empty());
  for (const auto &Block : Recovery.Residual.Blocks)
    for (const auto &Op : Block.Ops) {
      EXPECT_NE(Op.Opcode, NdOp::INDIR_BR);
      EXPECT_NE(Op.Opcode, NdOp::INDIR_CALL);
    }
  if (Kind == 2)
    EXPECT_TRUE(hasCycle(Recovery.Residual))
        << "A runtime-dependent loop must survive specialization";

  CEmitterOptions COptions;
  COptions.TheArch = Image->Arch;
  COptions.Format = Image->Format;
  COptions.Image = &*Image;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  if (LLVM) {
    ASSERT_NE(Result.LlvmModule, nullptr);
    ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, COptions, nullptr,
                                    &*Image));
  } else {
    ASSERT_EQ(Result.HighFuncs.size(), 1u);
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COptions));
  }
  OS.flush();
  ASSERT_FALSE(Source.empty());
  const auto Harness = tmpFile("generic-vm-recovered.c");
  std::ofstream(Harness)
      << Source << "\n#define GENERIC_VM_SOURCE\n#define GENERIC_VM_FUNCTION "
      << Name << "\n#define GENERIC_VM_KIND " << Kind << "\n"
      << readSource(fixture("generic_vm_reference.c"));
  // The fixtures contain only scalar x64 operations.
  std::ofstream(tmpFile("immintrin.h")).close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = tmpFile("generic-vm-recovered");
    const auto Recompiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-fno-inline", "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
  }
}

INSTANTIATE_TEST_SUITE_P(
    PublicMachines, DevirtualizationRoundTripTest,
    ::testing::Values(SourceCase{"generic_vm_register_arithmetic", 0, false},
                      SourceCase{"generic_vm_register_arithmetic", 0, true},
                      SourceCase{"generic_vm_register_branch", 1, false},
                      SourceCase{"generic_vm_register_branch", 1, true},
                      SourceCase{"generic_vm_register_loop", 2, false},
                      SourceCase{"generic_vm_register_loop", 2, true},
                      SourceCase{"generic_vm_stack_arithmetic", 0, false},
                      SourceCase{"generic_vm_stack_arithmetic", 0, true},
                      SourceCase{"generic_vm_stack_branch", 1, false},
                      SourceCase{"generic_vm_stack_branch", 1, true},
                      SourceCase{"generic_vm_stack_loop", 2, false},
                      SourceCase{"generic_vm_stack_loop", 2, true}),
    [](const ::testing::TestParamInfo<SourceCase> &Info) {
      return std::string(std::get<0>(Info.param)) +
             (std::get<2>(Info.param) ? "_LLVMC" : "_HighC");
    });

TEST_F(DevirtualizationSourceTest, RefusesUncertifiedControl) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM negative fixtures require clang";
  const auto Binary = tmpFile("generic-vm-negative.elf");
  const auto Compiled = buildFixture(Binary, "generic_vm_negative.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  using Status = analysis::SpecializationStatus;
  const std::pair<const char *, Status> Cases[] = {
      {"generic_vm_unresolved_dispatch", Status::UnresolvedControl},
      {"generic_vm_writable_dispatch", Status::UnresolvedControl},
      {"generic_vm_return_dispatch", Status::Unsupported},
      {"generic_vm_callee_pop", Status::Unsupported},
      {"generic_vm_far_return", Status::Unsupported},
      {"generic_vm_interrupt_return", Status::Unsupported},
      {"generic_vm_system_return", Status::Unsupported}};
  for (const auto &[Name, ExpectedStatus] : Cases) {
    SCOPED_TRACE(Name);
    const va_t Entry = functionEntry(*Image, Name);
    ASSERT_NE(Entry, InvalidVA);
    auto Recovery =
        analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
    EXPECT_FALSE(Recovery.complete());
    EXPECT_EQ(Recovery.Status, ExpectedStatus) << Recovery.Diagnostic;
    EXPECT_TRUE(Recovery.Residual.Blocks.empty());
    EXPECT_FALSE(Recovery.Diagnostic.empty());
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.InterpreterSpecialization = recoveryOptions();
    Options.OnlyFunctionEntries.insert(Entry);
    Options.EmitDumpOutput = false;
    const auto Result = Pipeline().run(*Image, Context, Options);
    EXPECT_FALSE(Result.Success);
    EXPECT_TRUE(Result.LowFuncs.empty());
    EXPECT_TRUE(Result.MedFuncs.empty());
    EXPECT_TRUE(Result.HighFuncs.empty());
    EXPECT_EQ(Result.LlvmModule, nullptr);
  }
}

TEST_F(DevirtualizationSourceTest, RefusesNonNativeByteOrder) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM image contracts require clang";
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_vm_register_arithmetic");
  ASSERT_NE(Entry, InvalidVA);
  auto Options = recoveryOptions();
  Options.ByteOrder = llvm::endianness::big;
  const auto Result =
      analysis::specializeBinaryInterpreter(*Image, Entry, Options);
  EXPECT_EQ(Result.Status, analysis::SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_FALSE(Result.Diagnostic.empty());
}

TEST_F(DevirtualizationSourceTest, RefusesUndecodedCoveringExceptionHandler) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM image contracts require clang";
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_vm_register_arithmetic");
  ASSERT_NE(Entry, InvalidVA);
  ExceptionFunction Parent;
  Parent.CodeRange = {Entry, Entry + 0x1000};
  Parent.PersonalityVA = Entry + 0x800;
  ASSERT_FALSE(Parent.hasLanguageTable());
  ExceptionFunction Child;
  Child.CodeRange = {Entry, Entry + 0x400};
  // The most specific range contains no decoded table. Its parent still has
  // a handler, so finding only the smallest record is insufficient.
  Image->ExceptionMetadata.Functions = {Parent, Child};
  Image->ExceptionMetadata.rebuildIndex();
  ASSERT_EQ(Image->ExceptionMetadata.findFunction(Entry)->PersonalityVA, 0u);
  const auto Result =
      analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
  EXPECT_EQ(Result.Status, analysis::SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_FALSE(Result.Diagnostic.empty());
}

TEST_F(DevirtualizationSourceTest, RefusesIncompleteExceptionMetadata) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM image contracts require clang";
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_vm_register_arithmetic");
  ASSERT_NE(Entry, InvalidVA);
  for (const auto Status :
       {ExceptionParseStatus::Partial, ExceptionParseStatus::Malformed}) {
    Image->ExceptionMetadata.ParseStatus = Status;
    const auto Result =
        analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
    EXPECT_EQ(Result.Status, analysis::SpecializationStatus::Unsupported);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_FALSE(Result.Diagnostic.empty());
  }
}

TEST_F(DevirtualizationSourceTest, CLIEmitsSourceAndEvidenceOnlyOnSuccess) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM CLI recovery requires clang";
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  const auto Source = tmpFile("generic-vm.c");
  const auto Report = tmpFile("generic-vm.json");
  const auto Recovered =
      exec(ndBin(), {"decompile", "--func", "generic_vm_register_arithmetic",
                     "--devirtualize", "--vm-control=r10",
                     "--recovery-report=" + Report.string(), "-o",
                     Source.string(), Binary.string()});
  ASSERT_TRUE(Recovered.ok()) << Recovered.err;
  EXPECT_FALSE(readSource(Source).empty());
  auto JSON = llvm::json::parse(readSource(Report));
  ASSERT_TRUE(static_cast<bool>(JSON)) << llvm::toString(JSON.takeError());
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(Object->getBoolean("complete"), true);
  EXPECT_EQ(Object->getBoolean("controlComplete"), true);
  const auto *Controls = Object->getArray("controlRegisters");
  ASSERT_NE(Controls, nullptr);
  ASSERT_EQ(Controls->size(), 1u);
  const auto *Control = Controls->front().getAsObject();
  ASSERT_NE(Control, nullptr);
  EXPECT_EQ(Control->getInteger("offset"), x86reg::R10);
  EXPECT_EQ(Control->getInteger("bytes"), 8);

  const auto InvalidSource = tmpFile("invalid.c");
  const auto InvalidReport = tmpFile("invalid.json");
  const auto Refused =
      exec(ndBin(), {"decompile", "--func", "generic_vm_register_arithmetic",
                     "--devirtualize", "--vm-control=bogus",
                     "--recovery-report=" + InvalidReport.string(), "-o",
                     InvalidSource.string(), Binary.string()});
  EXPECT_FALSE(Refused.ok());
  EXPECT_FALSE(fs::exists(InvalidSource));
  auto InvalidJSON = llvm::json::parse(readSource(InvalidReport));
  ASSERT_TRUE(static_cast<bool>(InvalidJSON))
      << llvm::toString(InvalidJSON.takeError());
  ASSERT_NE(InvalidJSON->getAsObject(), nullptr);
  EXPECT_EQ(InvalidJSON->getAsObject()->getBoolean("complete"), false);
}

TEST_F(DevirtualizationSourceTest, LLVMCMixedFrameViewsPreserveUntouchedBytes) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "mixed frame source execution requires clang";
  llvm::LLVMContext Context;
  llvm::Module Module("mixed-frame-views", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> Builder(Context);
  constexpr uint64_t Initial = UINT64_C(0xaabbccddee112233);
  for (unsigned Offset : {0u, 3u}) {
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getInt64Ty(), {Builder.getInt64Ty()},
                                false),
        llvm::GlobalValue::ExternalLinkage,
        "generic_vm_mixed_frame_" + std::to_string(Offset), Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Frame = Builder.CreateAlloca(
        llvm::ArrayType::get(Builder.getInt8Ty(), 64), nullptr, "frame");
    Builder.CreateGEP(Builder.getInt8Ty(), Frame, Builder.getInt64(32),
                      "frame_end");
    auto *Whole =
        Builder.CreateGEP(Builder.getInt8Ty(), Frame, Builder.getInt64(16));
    auto *Narrow = Builder.CreateGEP(Builder.getInt8Ty(), Frame,
                                     Builder.getInt64(16 + Offset));
    Builder.CreateStore(Builder.getInt64(Initial), Whole);
    Builder.CreateStore(
        Builder.CreateTrunc(Function->getArg(0), Builder.getInt8Ty()), Narrow);
    Builder.CreateRet(Builder.CreateLoad(Builder.getInt64Ty(), Whole));
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS));
  OS.flush();
  const auto Harness = tmpFile("mixed-frame.c");
  std::ofstream(Harness) << Source << R"(
int main(void) {
  static const uint64_t values[] = {0, 1, 0xab, UINT64_MAX};
  for (unsigned i = 0; i != 4; ++i) {
    const uint64_t initial = UINT64_C(0xaabbccddee112233);
    const uint64_t low = values[i] & UINT64_C(0xff);
    if (generic_vm_mixed_frame_0(values[i]) !=
        ((initial & ~UINT64_C(0xff)) | low))
      return 1;
    if (generic_vm_mixed_frame_3(values[i]) !=
        ((initial & ~(UINT64_C(0xff) << 24)) | (low << 24)))
      return 2;
  }
  return 0;
}
)";
  std::ofstream(tmpFile("immintrin.h")).close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = tmpFile("mixed-frame");
    const auto Recompiled = exec(
        NEVERD_TEST_CLANG, {"-std=c11", Optimization, "-fsanitize=undefined",
                            "-fsanitize-trap=undefined", "-I", tmp().string(),
                            Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
  }
}

TEST_F(DevirtualizationSourceTest, OriginalMachinesMatchIndependentOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM fixtures require clang";
#if !defined(__x86_64__) || !defined(__linux__)
  GTEST_SKIP() << "native x64 ELF execution requires an x64 Linux host";
#else
  for (bool MicrosoftABI : {false, true})
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(MicrosoftABI ? "Win64 ABI" : "SysV ABI");
      SCOPED_TRACE(Optimization);
      const auto Executable = tmpFile("generic-vm-native");
      std::vector<std::string> Args{"-std=c11",
                                    Optimization,
                                    "-no-pie",
                                    "-fsanitize=undefined",
                                    "-fsanitize-trap=undefined",
                                    fixture("generic_vm_register.S").string(),
                                    fixture("generic_vm_stack.S").string(),
                                    fixture("generic_vm_reference.c").string(),
                                    "-o",
                                    Executable.string()};
      if (MicrosoftABI)
        Args.push_back("-DGENERIC_VM_MS_ABI");
      const auto Compiled = exec(NEVERD_TEST_CLANG, Args);
      ASSERT_TRUE(Compiled.ok()) << Compiled.err;
      const auto Ran = exec(Executable.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err;
    }
#endif
}

TEST_F(DevirtualizationSourceTest, PublicMachinesAssembleForBothNativeABIs) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target public VM fixtures require clang";
  for (const char *Triple : {"x86_64-linux-gnu", "x86_64-pc-windows-msvc"}) {
    SCOPED_TRACE(Triple);
    for (const char *Name : {"generic_vm_register.S", "generic_vm_stack.S",
                             "generic_vm_negative.S"}) {
      SCOPED_TRACE(Name);
      const auto Compiled = exec(
          NEVERD_TEST_CLANG, {"-target", Triple, "-c", fixture(Name).string(),
                              "-o", tmpFile("generic-vm.o").string()});
      ASSERT_TRUE(Compiled.ok()) << Compiled.err;
    }
  }
}

} // namespace
