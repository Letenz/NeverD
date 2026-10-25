//===- IncomingHomeSourceTests.cpp - Incoming stack source values --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute source projections of explicit incoming stack carriers.

#include "../core/SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t Caller = 0x140001000;
constexpr va_t Callee = Caller + 0x40;

BinaryImage homeImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.Entry = Caller;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Caller;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0x48, 0x83, 0xec, 0x28,               // sub rsp,28h
               0x48, 0xc7, 0x04, 0x24, 42,  0, 0, 0, // mov qword [rsp],42
               0xe8, 0x2f, 0,    0,    0,            // call read_home
               0x48, 0x83, 0xc4, 0x28, 0xc3};        // add rsp,28h; ret
  Text.Data.resize(0x40, 0xcc);
  // CALL pushes its return address: the caller's outgoing byte zero is the
  // callee's entry-SP+8, even though it is below Win64's usual fifth argument.
  Text.Data.insert(Text.Data.end(), {0x48, 0x8b, 0x44, 0x24, 8, 0xc3});
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  for (const auto &[Address, Name] :
       {std::pair{Caller, "caller_home"}, std::pair{Callee, "read_home"}}) {
    auto Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    Image.Symbols.push_back(std::move(Function));
  }
  return Image;
}

SourceFunctionTypeHint homeSignature() {
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  Hint.Architecture = Arch::X64;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = NdType::makeInt(8, false);
  Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister, 0, 0, 8};
  SourceParameterTypeHint Parameter;
  Parameter.Name = "incoming_home";
  Parameter.Type = NdType::makeInt(8, false);
  Parameter.Location = {SourceABICarrierKind::Stack, 0, 8, 8};
  Hint.Parameters.push_back(std::move(Parameter));
  return Hint;
}

TEST(IncomingHomeSource, ExplicitStackCarrierReachesBothCPaths) {
  const auto Image = homeImage();
  const auto Signature = homeSignature();
  std::string Diagnostic;
  ASSERT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image));
  CFGBuilder Builder;
  const std::set<va_t> Entries{Caller, Callee};
  Builder.setKnownFuncEntries(&Entries);
  const auto Low = Builder.build(Image, Decoder, Caller, "caller_home");
  std::map<SourceCallOccurrenceKey, SourceCallTypeHint> Sites;
  for (const auto &Block : Low.Blocks)
    for (const auto &Operation : Block.Ops)
      if (const auto Site = sourceCallOccurrenceKey(Operation);
          Site && Site->StaticTarget == Callee) {
        SourceCallTypeHint Hint;
        Hint.CallKind = SourceCallTypeHint::Kind::Native;
        Hint.TargetAddress = Callee;
        Hint.TargetName = "read_home";
        Hint.Signature = Signature;
        Sites.emplace(*Site, std::move(Hint));
      }
  ASSERT_EQ(Sites.size(), 1u);
  for (bool LLVM : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = Entries;
      Options.SourceProjection = true;
      Options.LiftMode = LLVM;
      Options.NoOpt = NoOpt;
      Options.SourceTypeHints.emplace(Callee, Signature);
      Options.SourceCallSiteHints.emplace(Caller, Sites);
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      const auto Function = std::find_if(
          Result.MedFuncs.begin(), Result.MedFuncs.end(),
          [](const MedFunc &Function) { return Function.Entry == Callee; });
      ASSERT_NE(Function, Result.MedFuncs.end());
      ASSERT_TRUE(Function->SourceParametersBound);
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
      } else
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
      ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source + "\nint main(void) { return caller_home() != 42 || "
                     "read_home(71) != 71; }\n",
            {Level, "-ftrivial-auto-var-init=pattern", "-fsanitize=undefined",
             "-fsanitize-trap=all"});
    }
}
} // namespace
