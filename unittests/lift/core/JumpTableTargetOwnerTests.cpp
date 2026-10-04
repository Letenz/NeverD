#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Support/Endian.h"

using namespace neverd;

namespace {
BinaryImage compactDispatch(size_t UnrelatedFunctions,
                            size_t UnrelatedSymbols = 0) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Entry = 0x1000;
  Segment Text;
  Text.VA = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  auto Append = [&](uint32_t Instruction) {
    for (unsigned I = 0; I < 4; ++I)
      Text.Data.push_back(static_cast<uint8_t>(Instruction >> (8 * I)));
  };
  // sub w8,w0,#1; cmp w8,#16; b.hi default; materialize table/anchor;
  // ldrh w11,[x9,x8,lsl #1]; add x10,x10,x11,lsl #2; br x10.
  for (uint32_t Word :
       {0x51000408U, 0x7100411fU, 0x54000528U, 0xb0000009U, 0x91000129U,
        0x1000008aU, 0x7868792bU, 0x8b0b094aU, 0xd61f0140U})
    Append(Word);
  for (unsigned I = 0; I < 18; ++I) {
    Append(0x52800000U | ((100 + I) << 5));
    Append(0xd65f03c0U);
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Symbols.push_back(Symbol::makeFunc(Text.VA, Text.Size));
  Section Instructions;
  Instructions.VA = Text.VA;
  Instructions.Size = Text.Size;
  Instructions.Flags = Text.Flags;
  Instructions.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Instructions);
  Image.Segments.push_back(std::move(Text));
  Segment Table;
  Table.VA = 0x2000;
  Table.Flags = SegmentFlags::Readable;
  for (unsigned I = 0; I < 17; ++I) {
    Table.Data.push_back(static_cast<uint8_t>(I * 2));
    Table.Data.push_back(0);
  }
  Table.Size = Table.FileSz = Table.Data.size();
  Section Constants;
  Constants.VA = Table.VA;
  Constants.Size = Table.Size;
  Constants.Flags = Table.Flags;
  Image.Sections.push_back(Constants);
  Image.Segments.push_back(std::move(Table));
  for (size_t I = 0; I < UnrelatedFunctions; ++I) {
    ExceptionFunction Function;
    Function.Kind = RuntimeFunctionKind::Primary;
    Function.CodeRange = {0x100000 + I * 16, 0x100010 + I * 16};
    Image.ExceptionMetadata.Functions.push_back(std::move(Function));
  }
  for (size_t I = 0; I < UnrelatedSymbols; ++I)
    Image.Symbols.push_back(Symbol::makeFunc(0x2000000 + I * 16, 16));
  return Image;
}
} // namespace

TEST(JumpTableTargetOwners, IndexesFragmentOwnershipForLargeCompactDispatch) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF})
    for (auto [Count, Symbols] :
         {std::pair<size_t, size_t>{0, 0}, {60000, 0}, {40000, 200000}}) {
      SCOPED_TRACE(Count);
      SCOPED_TRACE(static_cast<int>(Format));
      auto Image = compactDispatch(Count, Symbols);
      Image.Format = Format;
      ExecutableCodeOwnerIndex Owners(Image);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image.Arch));
      CFGBuilder Builder;
      Builder.setExecutableCodeOwnerIndex(&Owners);
      const auto Low = Builder.build(Image, Dec, Image.Entry);
      EXPECT_EQ(Low.JumpTables.size(), 1U)
          << "blocks=" << Low.Blocks.size()
          << " unsafe=" << Low.UnsafeIndirectBranchAddresses.size();
      if (Low.JumpTables.empty())
        continue;
      ASSERT_EQ(Low.JumpTables.front().Targets.size(), 17U);
      EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
      for (unsigned I = 0; I < 17; ++I)
        EXPECT_EQ(Low.JumpTables.front().Targets[I], 0x1024U + I * 8);
      if (Count == 0) {
        Builder.setMaskFixedPointEvidenceBudgetForTesting(0);
        const auto Exhausted = Builder.build(Image, Dec, Image.Entry);
        EXPECT_TRUE(Exhausted.JumpTables.empty());
      }
    }
}

TEST(JumpTableTargetOwners, FragmentsKeepExactPrimaryRelationships) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF})
    for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
      for (bool Reverse : {false, true}) {
        BinaryImage Image;
        Image.Format = Format;
        Image.Arch = Architecture;
        Image.Mode = Architecture == Arch::ARM ? InstructionMode::Thumb
                                               : InstructionMode::Default;
        auto Add = [&](RuntimeFunctionKind Kind, va_t Begin, va_t End,
                       std::optional<size_t> Parent = {},
                       std::optional<va_t> Chained = {}) {
          ExceptionFunction Function;
          Function.Kind = Kind;
          Function.CodeRange = {Begin, End};
          Function.PrimaryFunctionIndex = Parent;
          if (Chained)
            Function.ChainedPrimaryRange = {*Chained, *Chained + 8};
          Image.ExceptionMetadata.Functions.push_back(std::move(Function));
        };
        Add(RuntimeFunctionKind::Primary, 0x1001, 0x1080);
        Add(RuntimeFunctionKind::Primary, 0x1001, 0x1040);
        Add(RuntimeFunctionKind::Primary, 0x2000, 0x2080);
        Add(RuntimeFunctionKind::Fragment, 0x3000, 0x3010, 0);
        Add(RuntimeFunctionKind::Chained, 0x3008, 0x3020, 1);
        Add(RuntimeFunctionKind::Fragment, 0x3010, 0x3030, 2);
        Add(RuntimeFunctionKind::Fragment, 0x4000, 0x4010, 999, 0x1001);
        Add(RuntimeFunctionKind::Fragment, 0x5000, 0x5010, 3, 0x9000);
        Add(RuntimeFunctionKind::Primary, 0x6000, 0x6010, 0);
        Add(RuntimeFunctionKind::Fragment, 0x7010, 0x7000, 0);
        Add(RuntimeFunctionKind::Fragment, 0x8000, 0x8010, 2, 0x1001);
        if (Reverse) {
          auto &Functions = Image.ExceptionMetadata.Functions;
          for (auto &Function : Functions)
            if (Function.PrimaryFunctionIndex &&
                *Function.PrimaryFunctionIndex < Functions.size())
              Function.PrimaryFunctionIndex =
                  Functions.size() - 1 - *Function.PrimaryFunctionIndex;
          std::reverse(Functions.begin(), Functions.end());
        }
        const ExecutableCodeOwnerIndex Owners(Image);
        BinaryImage Other;
        const ExecutableCodeOwnerIndex Foreign(Other);
        for (va_t Entry : {0x1000U, 0x1001U, 0x2000U, 0x6000U, 0x9000U})
          for (va_t Target :
               {0x1001U, 0x2fffU, 0x3000U, 0x3008U, 0x3010U, 0x301fU, 0x3020U,
                0x302fU, 0x3030U, 0x4000U, 0x400fU, 0x4010U, 0x5000U, 0x6000U,
                0x7000U, 0x7010U, 0x8000U, 0x800fU, 0x8010U}) {
            const bool Expected =
                (Entry == 0x1001 && ((Target >= 0x3000 && Target < 0x3020) ||
                                     (Target >= 0x4000 && Target < 0x4010) ||
                                     (Target >= 0x8000 && Target < 0x8010))) ||
                (Entry == 0x2000 && ((Target >= 0x3010 && Target < 0x3030) ||
                                     (Target >= 0x8000 && Target < 0x8010)));
            for (const auto *Index :
                 {static_cast<const ExecutableCodeOwnerIndex *>(nullptr),
                  &Owners, &Foreign})
              EXPECT_EQ(isExplicitlyOwnedFunctionFragment(Image, Entry, Target,
                                                          Index),
                        Expected)
                  << "entry=" << Entry << " target=" << Target;
          }
      }
}

TEST(JumpTableTargetOwners, NewImageOperationsObserveChangedFragmentOwners) {
  BinaryImage Image;
  ExceptionFunction Primary;
  Primary.Kind = RuntimeFunctionKind::Primary;
  Primary.CodeRange = {0x1000, 0x1080};
  Image.ExceptionMetadata.Functions.push_back(Primary);
  Primary.CodeRange = {0x2000, 0x2080};
  Image.ExceptionMetadata.Functions.push_back(Primary);
  ExceptionFunction Fragment;
  Fragment.Kind = RuntimeFunctionKind::Fragment;
  Fragment.CodeRange = {0x3000, 0x3010};
  Fragment.PrimaryFunctionIndex = 0;
  Image.ExceptionMetadata.Functions.push_back(Fragment);
  const auto Original = Image;
  const ExecutableCodeOwnerIndex OriginalOwners(Original);
  EXPECT_TRUE(isExplicitlyOwnedFunctionFragment(Original, 0x1000, 0x3008,
                                                &OriginalOwners));
  Image.ExceptionMetadata.Functions.back().PrimaryFunctionIndex = 1;
  const ExecutableCodeOwnerIndex UpdatedOwners(Image);
  EXPECT_FALSE(OriginalOwners.fragmentLookupWork(Image).has_value());
  EXPECT_TRUE(UpdatedOwners.fragmentLookupWork(Image).has_value());
  for (const auto *Index : {&OriginalOwners, &UpdatedOwners}) {
    EXPECT_FALSE(
        isExplicitlyOwnedFunctionFragment(Image, 0x1000, 0x3008, Index));
    EXPECT_TRUE(
        isExplicitlyOwnedFunctionFragment(Image, 0x2000, 0x3008, Index));
    EXPECT_FALSE(
        isExplicitlyOwnedFunctionFragment(Image, 0x2000, 0x3010, Index));
  }
}

TEST(JumpTableTargetOwners, IndexedTargetsRejectIndependentFunctionEntries) {
  auto Image = compactDispatch(0);
  Image.Symbols.push_back(Symbol::makeFunc(0x102c, 8));
  const std::set<va_t> Entries{0x1000, 0x102c};
  const ExecutableCodeOwnerIndex Owners(Image);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image.Arch));
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  Builder.setExecutableCodeOwnerIndex(&Owners);
  const auto Low = Builder.build(Image, Dec, Image.Entry);
  EXPECT_TRUE(Low.JumpTables.empty());
}

namespace {
BinaryImage selectedFunctionBoundaryFixture(BinaryFormat Format,
                                            Arch Architecture, bool Tail,
                                            unsigned Evidence) {
  BinaryImage Image;
  Image.Format = Format;
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                   ? Bitness::Bits32
                   : Bitness::Bits64;
  if (Architecture == Arch::ARM)
    Image.Mode = InstructionMode::ARM;
  Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Size = Text.FileSz = 0x18;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  if (Architecture == Arch::AArch64) {
    llvm::support::endian::write32le(Text.Data.data(),
                                     Tail ? 0x14000004 : 0xd65f03c0);
    llvm::support::endian::write32le(Text.Data.data() + 0x10, 0x52800540);
    llvm::support::endian::write32le(Text.Data.data() + 0x14, 0xd65f03c0);
  } else if (Architecture == Arch::ARM) {
    llvm::support::endian::write32le(Text.Data.data(),
                                     Tail ? 0xea000002 : 0xe12fff1e);
    llvm::support::endian::write32le(Text.Data.data() + 0x10, 0xe3a0002a);
    llvm::support::endian::write32le(Text.Data.data() + 0x14, 0xe12fff1e);
  } else {
    Text.Data[0] = Tail ? 0xe9 : 0xc3;
    if (Tail)
      llvm::support::endian::write32le(Text.Data.data() + 1, 0xb);
    Text.Data[0x10] = 0xb8;
    Text.Data[0x11] = 42;
    Text.Data[0x15] = 0xc3;
  }
  Section Sec;
  Sec.VA = Text.VA;
  Sec.Size = Text.Size;
  Sec.Flags = Text.Flags;
  Sec.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Sec);
  Image.Segments.push_back(std::move(Text));
  Segment Table;
  Table.VA = 0x2000;
  Table.Size = Table.FileSz = 8;
  Table.Flags = SegmentFlags::Readable;
  Table.Data.resize(8);
  llvm::support::endian::write64le(Table.Data.data(), 0x1010);
  Image.Segments.push_back(std::move(Table));
  Image.CodePtrRelocSlots.insert(0x2000);
  Image.Symbols.push_back(Symbol::makeFunc(Image.Entry));
  auto Other = Symbol::makeFunc(0x1010);
  Other.IsFunc = Evidence != 1;
  Other.IsBoundaryGuess = Evidence == 2;
  if (Evidence != 3)
    Image.Symbols.push_back(Other);
  ExceptionFunction Shared;
  Shared.Kind = RuntimeFunctionKind::Primary;
  Shared.ParseStatus = ExceptionParseStatus::Complete;
  Shared.CodeRange = {0x1000, 0x1018};
  Image.ExceptionMetadata.Functions.push_back(Shared);
  Image.ExceptionMetadata.rebuildIndex();
  return Image;
}
} // namespace

TEST(JumpTableTargetOwners, SelectedFunctionsKeepIndependentSymbolBoundaries) {
  for (auto Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (auto Architecture : {Arch::AArch64, Arch::X64, Arch::ARM, Arch::X86})
      for (bool Tail : {false, true})
        for (bool Reverse : {false, true}) {
          SCOPED_TRACE(static_cast<int>(Format));
          SCOPED_TRACE(static_cast<int>(Architecture));
          SCOPED_TRACE(Tail);
          SCOPED_TRACE(Reverse);
          // A shared unwind range does not join independently callable
          // entries, even when an absolute relocation names the second one.
          auto Image =
              selectedFunctionBoundaryFixture(Format, Architecture, Tail, 0);
          if (Reverse)
            std::reverse(Image.Symbols.begin(), Image.Symbols.end());
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          auto SelectedImage = Image;
          auto Full = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Full.Success) << Full.Error;
          auto First = std::find_if(
              Full.LowFuncs.begin(), Full.LowFuncs.end(),
              [&](const LowFunc &F) { return F.Entry == Image.Entry; });
          ASSERT_NE(First, Full.LowFuncs.end());
          EXPECT_TRUE(
              std::any_of(Full.LowFuncs.begin(), Full.LowFuncs.end(),
                          [](const LowFunc &F) { return F.Entry == 0x1010; }));
          Options.OnlyFunctionEntries = {Image.Entry};
          auto Selected = Pipeline().run(SelectedImage, Context, Options);
          ASSERT_TRUE(Selected.Success) << Selected.Error;
          ASSERT_EQ(Selected.LowFuncs.size(), 1U);
          const auto &Low = Selected.LowFuncs.front();
          EXPECT_EQ(Low.Entry, Image.Entry);
          ASSERT_EQ(Low.Blocks.size(), 1U);
          EXPECT_EQ(Low.ModuleAnalysisRoots, std::set<va_t>{Image.Entry});
          EXPECT_EQ(Low.OrdinaryModuleAnalysisRoots,
                    First->OrdinaryModuleAnalysisRoots);
          std::string AllBody, SelectedBody;
          llvm::raw_string_ostream AllStream(AllBody),
              SelectedStream(SelectedBody);
          Pipeline::dumpLowIR({*First}, AllStream);
          Pipeline::dumpLowIR({Low}, SelectedStream);
          EXPECT_EQ(SelectedBody, AllBody);
          unsigned Calls = 0;
          for (const auto &Operation : Low.Blocks.front().Ops) {
            EXPECT_LT(Operation.Addr, 0x1010U);
            if (Operation.Opcode == NdOp::CALL) {
              ++Calls;
              ASSERT_TRUE(Operation.Inputs[0].isConst());
              EXPECT_EQ(Operation.Inputs[0].Offset, 0x1010U);
            }
          }
          EXPECT_EQ(Calls, Tail ? 1U : 0U);
        }
}

TEST(JumpTableTargetOwners, SelectedFunctionsKeepUnprovenInteriorRoots) {
  for (auto Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (auto Architecture : {Arch::AArch64, Arch::X64, Arch::ARM, Arch::X86})
      for (bool Tail : {false, true})
        for (unsigned Evidence : {1U, 2U, 3U}) {
          SCOPED_TRACE(static_cast<int>(Format));
          SCOPED_TRACE(static_cast<int>(Architecture));
          SCOPED_TRACE(Tail);
          SCOPED_TRACE(Evidence);
          // A nonfunction symbol, padding guess, or absent symbol does not
          // remove an address-taken local block from the current CFG.
          auto Image = selectedFunctionBoundaryFixture(Format, Architecture,
                                                       Tail, Evidence);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {Image.Entry};
          auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          ASSERT_EQ(Result.LowFuncs.size(), 1U);
          const auto &Low = Result.LowFuncs.front();
          EXPECT_TRUE(Low.ModuleAnalysisRoots.count(0x1010));
          EXPECT_TRUE(std::any_of(
              Low.Blocks.begin(), Low.Blocks.end(),
              [](const LowBlock &B) { return B.StartAddr == 0x1010; }));
        }
}
