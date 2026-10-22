//===- BinaryDiffDataFlowPipelineTests.cpp - Native SSA diff evidence -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owned native bytes through the real decoder, strict lifter and SSA owner.
/// These fixtures are analyzed without executing the target instructions.
//===----------------------------------------------------------------------===//

#include "neverd/diff/MedDataFlow.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <gtest/gtest.h>

using namespace neverd;
using namespace neverd::diff;
namespace {
DataFlowGraph nativeGraph(Arch Architecture, std::vector<uint8_t> Code,
                          uint64_t Base, bool ImportStub = false) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Base = Image.Entry = Base;
  Segment Segment;
  Segment.VA = Base;
  Segment.Size = Code.size();
  Segment.Data = std::move(Code);
  Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Segment));
  Image.Symbols.push_back(Symbol::makeFunc(Base));
  if (ImportStub)
    Image.ImportStubRanges.emplace_back(Base, Base + Image.Segments[0].Size);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.OnlyFunctionEntries.insert(Base);
  Options.MedAnalysisOnly = true;
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(Image, Context, Options);
  EXPECT_TRUE(Result.Success) << Result.Error;
  EXPECT_TRUE(Result.HighFuncs.empty());
  EXPECT_FALSE(Result.HighIRDeferred);
  EXPECT_EQ(Result.LlvmModule, nullptr);
  const auto Audit = llvm::find_if(
      Result.FunctionAudits, [&](const auto &A) { return A.Entry == Base; });
  EXPECT_NE(Audit, Result.FunctionAudits.end());
  if (Audit != Result.FunctionAudits.end()) {
    EXPECT_TRUE(Audit->MedIRVerified);
    EXPECT_TRUE(Audit->DecodeFailures.empty());
    EXPECT_TRUE(Audit->UnsupportedInstructions.empty());
  }
  const auto F = llvm::find_if(Result.MedFuncs,
                               [&](const auto &F) { return F.Entry == Base; });
  if (F == Result.MedFuncs.end()) {
    ADD_FAILURE() << "pipeline did not retain the selected native function";
    return {};
  }
  return buildMedDataFlow(*F, "owned-fixture-v1",
                          Architecture == Arch::X64 ? "sysv-x64" : "aapcs64",
                          true);
}
std::vector<uint8_t> x64(uint8_t Add = 3) {
  // return (((unsigned)x + Add) ^ 23) * 7 + 9;
  return {0x89, 0xf8, 0x83, 0xc0, Add,  0x83, 0xf0, 0x17,
          0x6b, 0xc0, 0x07, 0x83, 0xc0, 0x09, 0xc3};
}
uint32_t operation(const DataFlowGraph &G, llvm::StringRef Opcode) {
  for (uint32_t N = 0; N < G.Nodes.size(); ++N)
    if (G.Nodes[N].Opcode == Opcode)
      return N;
  ADD_FAILURE() << "missing operation " << Opcode.str();
  return NoIndex;
}
} // namespace

TEST(BinaryDiffDataFlowPipeline, X64RebaseKeepsSSAKeysAndScalarChangesSurvive) {
  const auto A = nativeGraph(Arch::X64, x64(), 0x1000);
  const auto B = nativeGraph(Arch::X64, x64(), 0x8000);
  const auto C = nativeGraph(Arch::X64, x64(5), 0x8000);
  const auto AMul = operation(A, "INT_MULT"), BMul = operation(B, "INT_MULT"),
             CMul = operation(C, "INT_MULT");
  ASSERT_NE(AMul, NoIndex);
  ASSERT_NE(BMul, NoIndex);
  ASSERT_NE(CMul, NoIndex);
  EXPECT_EQ(A.Nodes[AMul].Address, 0x1008u);
  EXPECT_EQ(B.Nodes[BMul].Address, 0x8008u);
  EXPECT_EQ(fingerprintDataFlow(A).Keys[AMul],
            fingerprintDataFlow(B).Keys[BMul]);
  DataFlowOptions Deep;
  Deep.Rounds = 16;
  EXPECT_NE(fingerprintDataFlow(A, Deep).Keys[AMul],
            fingerprintDataFlow(C, Deep).Keys[CMul]);
  EXPECT_TRUE(fingerprintDataFlow(A).OpenDataFrontiers[AMul]);
  EXPECT_FALSE(fingerprintDataFlow(A, Deep).OpenDataFrontiers[AMul]);
  const auto AAdd = operation(A, "INT_ADD"), CAdd = operation(C, "INT_ADD");
  ASSERT_NE(AAdd, NoIndex);
  ASSERT_NE(CAdd, NoIndex);
  EXPECT_NE(fingerprintDataFlow(A).Keys[AAdd],
            fingerprintDataFlow(C).Keys[CAdd]);
  for (uint32_t N = 0; N < A.Nodes.size(); ++N)
    for (const auto &Input : A.Nodes[N].Inputs)
      if (Input.Type == FlowOperand::Kind::Node)
        EXPECT_NE(Input.Node, N); // An entry declaration is not a data cycle.
  const auto Slice = sliceDataFlow(C, {CMul}, SliceDirection::Backward, true);
  EXPECT_EQ(Slice.State, ComparisonState::Complete);
  EXPECT_TRUE(llvm::any_of(Slice.Nodes, [&](auto N) {
    return C.Nodes[N].Address == 0x8002 &&
           llvm::any_of(C.Nodes[N].Inputs, [](const auto &I) {
             return I.Type == FlowOperand::Kind::Constant && I.Value == 5;
           });
  }));
}

TEST(BinaryDiffDataFlowPipeline, AArch64KeepsNativeOriginsAndItsOwnABI) {
  std::vector<uint8_t> Code;
  for (uint32_t Word : {0x11000c00u, 0x528002e1u, 0x4a010000u, 0x528000e1u,
                        0x1b017c00u, 0x11002400u, 0xd65f03c0u})
    for (unsigned I = 0; I < 4; ++I)
      Code.push_back(uint8_t(Word >> (I * 8)));
  const auto G = nativeGraph(Arch::AArch64, Code, 0x4000);
  const auto Mul = operation(G, "INT_MULT");
  ASSERT_NE(Mul, NoIndex);
  EXPECT_EQ(G.ABI, "aapcs64");
  EXPECT_EQ(G.Nodes[Mul].Address, 0x4010u);
  const auto Slice = sliceDataFlow(G, {Mul}, SliceDirection::Backward, false);
  EXPECT_TRUE(llvm::any_of(
      Slice.Nodes, [&](auto N) { return G.Nodes[N].Address == 0x4000; }));
  EXPECT_EQ(fingerprintDataFlow(G).State, ComparisonState::Complete);
}

TEST(BinaryDiffDataFlowPipeline, SystemVCallArgumentsRetainSetupDependencies) {
  // push rax; lea edi,[rdi+3]; call helper; pop rcx; ret;
  // helper: lea eax,[rdi+5]; ret. Only the caller is selected; the pipeline's
  // existing omitted-callee recovery supplies the helper's argument contract.
  const auto G = nativeGraph(
      Arch::X64, {0x50, 0x8d, 0x7f, 0x03, 0xe8, 0x07, 0x00, 0x00, 0x00, 0x59,
                  0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x8d, 0x47, 0x05, 0xc3},
      0x1000);
  const auto Call = operation(G, "CALL");
  ASSERT_NE(Call, NoIndex);
  EXPECT_EQ(G.Nodes[Call].Address, 0x1004u);
  ASSERT_NE(G.Nodes[Call].CallArgumentBegin, NoIndex);
  ASSERT_EQ(G.Nodes[Call].CallArgumentCount, 1u);
  const auto &Argument = G.Nodes[Call].Inputs[G.Nodes[Call].CallArgumentBegin];
  ASSERT_EQ(Argument.Type, FlowOperand::Kind::Node);
  const auto Slice =
      sliceDataFlow(G, {Argument.Node}, SliceDirection::Backward, false);
  EXPECT_EQ(Slice.State, ComparisonState::Complete);
  EXPECT_TRUE(llvm::any_of(Slice.Nodes, [&](auto N) {
    return G.Nodes[N].Address == 0x1001 &&
           llvm::any_of(G.Nodes[N].Inputs, [](const auto &I) {
             return I.Type == FlowOperand::Kind::Constant && I.Value == 3;
           });
  }));
  EXPECT_TRUE(G.Nodes[Call].Gaps & FlowCallEffects);
  EXPECT_TRUE(G.Nodes[Call].Gaps & FlowMemoryAlias);
}

TEST(BinaryDiffDataFlowPipeline, AnalysisRetainsLoaderMarkedImportStubBodies) {
  // Import-stub exclusion belongs to native replacement output. A read-only
  // analysis still retains verified instructions in the loader-marked range.
  const auto G = nativeGraph(Arch::X64, x64(), 0x1000, true);
  EXPECT_NE(operation(G, "INT_MULT"), NoIndex);
}

TEST(BinaryDiffDataFlowPipeline, AnalysisRejectsVMsAndConflictingOutputModes) {
  llvm::LLVMContext Context;
  BinaryImage Image;
  PipelineOptions Options;
  Options.MedAnalysisOnly = true;
  for (auto Architecture : {Arch::EVM, Arch::SBF}) {
    Image.Arch = Architecture;
    const auto Result = Pipeline().run(Image, Context, Options);
    EXPECT_FALSE(Result.Success);
    EXPECT_FALSE(Result.Error.empty());
  }
  Image.Arch = Arch::X64;
  for (auto Mode : {&PipelineOptions::PatchMode, &PipelineOptions::LiftMode,
                    &PipelineOptions::DeferHighIR, &PipelineOptions::DeferMedIR,
                    &PipelineOptions::DumpHigh, &PipelineOptions::DumpLlvm,
                    &PipelineOptions::SourceProjection}) {
    auto Conflict = Options;
    Conflict.*Mode = true;
    const auto Result = Pipeline().run(Image, Context, Conflict);
    EXPECT_FALSE(Result.Success);
    EXPECT_FALSE(Result.Error.empty());
  }
}
