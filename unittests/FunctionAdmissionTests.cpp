//===- FunctionAdmissionTests.cpp - Decoded function admission
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Function discovery, unwind metadata and strict instruction admission.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {

BinaryImage makeCodeFixture(va_t Entry, std::vector<uint8_t> Bytes) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Segment Seg;
  Seg.VA = Entry;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Bytes);
  Img.Segments.push_back(std::move(Seg));
  return Img;
}

TEST(FunctionAdmission, UnwindlessNonLeafCandidateRemainsAFunction) {
  // An exception directory need not describe every executable entry. Neither
  // a stack adjustment nor a call disproves a candidate outside its records.
  constexpr va_t Primary = 0x140001000;
  constexpr va_t NonLeaf = 0x140001010;
  constexpr va_t Leaf = 0x140001020;
  std::vector<uint8_t> Code = {0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53,                         // push rbx
                           0x48, 0x83, 0xec, 0x20,       // sub rsp, 20h
                           0xe8, 0x06, 0x00, 0x00, 0x00, // call Leaf
                           0x48, 0x83, 0xc4, 0x20,       // add rsp, 20h
                           0x5b, 0xc3});                 // pop rbx; ret
  ASSERT_EQ(Code.size(), 0x20u);
  Code.insert(Code.end(), {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3}); // mov eax,1
  BinaryImage Img = makeCodeFixture(Primary, Code);
  Img.KnownCodeRanges.push_back({Primary, Primary + 1});
  for (va_t Entry : {Primary, NonLeaf, Leaf})
    Img.Symbols.push_back(Symbol::makeFunc(Entry));

  for (bool Selected : {false, true})
    for (bool Cached : {false, true}) {
      SCOPED_TRACE(Selected);
      SCOPED_TRACE(Cached);
      PipelineAnalysisCache Cache;
      for (va_t Entry : {NonLeaf, Leaf, NonLeaf}) {
        llvm::LLVMContext Ctx;
        PipelineOptions Opts;
        Opts.EmitDumpOutput = false;
        if (Selected)
          Opts.OnlyFunctionEntries.insert(Entry);
        if (Cached)
          Opts.AnalysisCache = &Cache;
        auto Result = Pipeline().run(Img, Ctx, Opts);
        ASSERT_TRUE(Result.Error.empty()) << Result.Error;
        const auto Audit = llvm::find_if(
            Result.FunctionAudits,
            [&](const PipelineFunctionAudit &A) { return A.Entry == Entry; });
        ASSERT_NE(Audit, Result.FunctionAudits.end());
        EXPECT_EQ(Audit->Disposition, PipelineFunctionDisposition::Accepted);
        EXPECT_TRUE(Audit->DecodeFailures.empty());
        EXPECT_TRUE(Audit->UnsupportedInstructions.empty());
        EXPECT_EQ(Audit->DecodedInstructions, Audit->LiftedInstructions);
        EXPECT_TRUE(llvm::any_of(Result.HighFuncs, [&](const HighFunc &F) {
          return F.Entry == Entry;
        }));
      }
    }
}

TEST(FunctionAdmission, UnwindRecordOrderDoesNotVetoSelectedCode) {
  constexpr va_t Primary = 0x140001000;
  constexpr va_t NonLeaf = 0x140001010;
  std::vector<uint8_t> Code = {0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53, 0x5b, 0xc3}); // push rbx; pop rbx; ret
  const auto DispositionOf = [&](std::vector<std::pair<va_t, va_t>> Ranges,
                                 std::set<va_t> Continuations) {
    BinaryImage Img = makeCodeFixture(Primary, Code);
    Img.KnownCodeRanges = std::move(Ranges);
    Img.ContinuationCodeStarts = std::move(Continuations);
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries.insert(NonLeaf);
    auto Result = Pipeline().run(Img, Ctx, Opts);
    for (const auto &Audit : Result.FunctionAudits)
      if (Audit.Entry == NonLeaf)
        return Audit.Disposition;
    return PipelineFunctionDisposition::Candidate;
  };
  const std::pair<va_t, va_t> Other{Primary, Primary + 1};
  const std::pair<va_t, va_t> Own{NonLeaf, NonLeaf + 3};
  // Primary and continuation records retain their ownership roles, but none
  // is required merely to decode an explicitly selected entry.
  EXPECT_EQ(DispositionOf({}, {}), PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Other}, {}), PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Other, Own}, {}),
            PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Own, Other}, {}),
            PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Other, Own}, {NonLeaf}),
            PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Own, Other}, {NonLeaf}),
            PipelineFunctionDisposition::Accepted);
}

TEST(FunctionAdmission, UnwindlessJumpTargetsUseActualChunkOwnership) {
  constexpr va_t Primary = 0x140001000;
  constexpr va_t Target = Primary + 0x10;
  std::vector<uint8_t> Code = {0xe9, 0x0b, 0x00, 0x00, 0x00}; // jmp Target
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53, 0xb8, 7, 0, 0, 0, 0x5b, 0xc3});
  Code.resize(0x30, 0xcc);
  Code.push_back(0xc3);

  for (bool Owned : {false, true})
    for (bool Guessed : {false, true}) {
      SCOPED_TRACE(Owned);
      SCOPED_TRACE(Guessed);
      BinaryImage Img = makeCodeFixture(Primary, Code);
      Img.Entry = Primary;
      Img.KnownCodeRanges.emplace_back(Primary, Primary + 5);
      Img.KnownCodeRanges.emplace_back(Primary + 0x30, Primary + 0x31);
      if (Owned) {
        ExceptionFunction Owner;
        Owner.Kind = RuntimeFunctionKind::Primary;
        Owner.ParseStatus = ExceptionParseStatus::Complete;
        Owner.CodeRange = {Primary, Primary + 5};
        Owner.FragmentRanges.push_back({Target, Target + 8});
        Img.ExceptionMetadata.Functions.push_back(std::move(Owner));
        Img.ExceptionMetadata.rebuildIndex();
      }
      Img.Symbols.push_back(Symbol::makeFunc(Primary));
      Img.Symbols.push_back(Symbol::makeFunc(Primary + 0x30));
      Symbol TargetSymbol = Symbol::makeFunc(Target);
      TargetSymbol.IsBoundaryGuess = Guessed;
      Img.Symbols.push_back(TargetSymbol);
      llvm::LLVMContext Ctx;
      PipelineOptions Opts;
      Opts.EmitDumpOutput = false;
      auto Result = Pipeline().run(Img, Ctx, Opts);
      ASSERT_TRUE(Result.Error.empty()) << Result.Error;
      const auto Audit = llvm::find_if(
          Result.FunctionAudits,
          [&](const PipelineFunctionAudit &A) { return A.Entry == Target; });
      ASSERT_NE(Audit, Result.FunctionAudits.end());
      const bool Absorbed = Guessed && Owned;
      EXPECT_EQ(Audit->Disposition,
                Absorbed ? PipelineFunctionDisposition::AbsorbedFunctionChunk
                         : PipelineFunctionDisposition::Accepted);
      // Absorption needs the target's actual decoded instructions in its owner.
      // A padding guess alone is no proof that it belongs to the caller.
      const va_t Owner = Absorbed ? Primary : Target;
      const auto Low = llvm::find_if(
          Result.LowFuncs, [&](const LowFunc &F) { return F.Entry == Owner; });
      ASSERT_NE(Low, Result.LowFuncs.end());
      EXPECT_TRUE(Low->hasCompleteInstructionLift());
      EXPECT_TRUE(llvm::any_of(Low->Blocks, [&](const LowBlock &B) {
        return llvm::any_of(B.InstructionBoundaries,
                            [&](const LowInstructionBoundary &I) {
                              return I.Address == Target;
                            });
      }));
      EXPECT_TRUE(llvm::any_of(Result.HighFuncs, [&](const HighFunc &F) {
        return F.Entry == Owner;
      }));
    }
}

TEST(FunctionAdmission, UnwindlessEntriesStillRequireCompleteDecoding) {
  constexpr va_t Primary = 0x140001000;
  constexpr va_t Broken = Primary + 0x10;
  std::vector<uint8_t> Code = {0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53, 0x0f}); // push rbx; truncated opcode
  BinaryImage Img = makeCodeFixture(Primary, Code);
  Img.KnownCodeRanges.emplace_back(Primary, Primary + 1);
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(Broken);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_EQ(Result.FunctionAudits.size(), 1u);
  const auto &Audit = Result.FunctionAudits.front();
  EXPECT_EQ(Audit.Disposition, PipelineFunctionDisposition::RejectedIncomplete);
  EXPECT_EQ(Audit.DecodeFailures, std::vector<va_t>{Broken + 1});
  EXPECT_TRUE(Result.LowFuncs.empty());
  EXPECT_TRUE(Result.HighFuncs.empty());
}

} // namespace
} // namespace neverd
