#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"

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
