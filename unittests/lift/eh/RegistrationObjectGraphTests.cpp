//===- RegistrationObjectGraphTests.cpp - PE32 object graphs --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify transitive object effects without lending the runtime caller frame.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <cstdlib>
#include <set>

namespace {
using namespace neverd;

struct ObjectGraphImage {
  BinaryImage Image;
  static constexpr va_t TableVA = 0x80000;
  static constexpr va_t DataVA = 0x90000;

  explicit ObjectGraphImage(unsigned Count = 3) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Entry = address(0);
    Segment Text;
    Text.VA = Image.Entry;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Count * 0x100, 0xcc);
    Text.Size = Text.FileSz = Text.Data.size();
    Section Code;
    Code.Name = ".text";
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    Image.Sections.push_back(Code);
    Image.Segments.push_back(std::move(Text));
    for (unsigned Index = 0; Index != Count; ++Index)
      Image.Symbols.push_back(Symbol::makeFunc(address(Index)));
    Segment Table;
    Table.VA = TableVA;
    Table.Flags = SegmentFlags::Readable;
    Table.Data.resize(0x100);
    Table.Size = Table.FileSz = Table.Data.size();
    for (const auto &[Offset, Value] : {std::pair{4u, uint32_t(address(0))},
                                        {12u, uint32_t(TableVA + 0x20)},
                                        {0x20u, 1u},
                                        {0x24u, uint32_t(TableVA + 0x30)},
                                        {0x34u, uint32_t(TableVA + 0x80)},
                                        {0x3cu, UINT32_MAX},
                                        {0x44u, 12u}})
      writeLE<uint32_t>(Table.Data.data() + Offset, Value);
    const std::string Name = ".?AUObjectGraph@@";
    llvm::copy(Name, Table.Data.begin() + 0x88);
    Image.Segments.push_back(std::move(Table));
    Segment Data;
    Data.VA = DataVA;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(16);
    Data.Size = Data.FileSz = Data.Data.size();
    Image.Segments.push_back(std::move(Data));
  }

  static va_t address(unsigned Index) { return 0x1000 + Index * 0x100; }

  void install(unsigned Index, llvm::ArrayRef<uint8_t> Code) {
    ASSERT_LE(Code.size(), 0x100u);
    ASSERT_LT(Index * 0x100, Image.Segments[0].Data.size());
    llvm::copy(Code, Image.Segments[0].Data.begin() + Index * 0x100);
  }

  void leaf(unsigned Index) {
    // The result deliberately retains entry EAX; runtime destruction is void.
    install(Index, {0xff, 0x01, 0xff, 0x05, 0, 0, 9, 0, 0xc3});
  }

  std::vector<uint8_t> wrapper(unsigned Index, llvm::ArrayRef<unsigned> Targets,
                               llvm::ArrayRef<uint8_t> Before = {},
                               llvm::ArrayRef<uint8_t> After = {}) {
    std::vector<uint8_t> Code = {0x56, 0x89,
                                 0xce}; // Save this in preserved ESI.
    for (unsigned Target : Targets) {
      Code.insert(Code.end(), {0x89, 0xf1});
      Code.insert(Code.end(), Before.begin(), Before.end());
      Code.push_back(0xe8);
      const size_t Offset = Code.size();
      Code.resize(Offset + 4);
      writeLE<uint32_t>(
          Code.data() + Offset,
          uint32_t(address(Target) - (address(Index) + Code.size())));
      Code.insert(Code.end(), After.begin(), After.end());
    }
    Code.insert(Code.end(), {0x5e, 0xc3});
    return Code;
  }

  std::optional<RegistrationThrowObjectABI>
  prove(size_t *Work = nullptr) const {
    return getCheckedX86RegistrationThrowObjectABI(Image, TableVA, Work);
  }
};

TEST(RegistrationObjectGraph, ComposesDestructionAndTransitiveCode) {
  ObjectGraphImage F;
  F.install(0, F.wrapper(0, {1, 1}));
  F.install(1, F.wrapper(1, {2}));
  F.leaf(2);
  const auto Proof = F.prove();
  ASSERT_TRUE(Proof);
  ASSERT_TRUE(Proof->Destructor);
  const auto &ABI = *Proof->Destructor;
  EXPECT_FALSE(ABI.HasIndependentScalarReturn);
  EXPECT_EQ(ABI.ECXReads, (std::vector<RegistrationObjectExtent>{{0, 4}}));
  EXPECT_EQ(ABI.ECXWrites, ABI.ECXReads);
  ASSERT_EQ(ABI.ImageReads.size(), 1u);
  ASSERT_EQ(ABI.ImageWrites.size(), 1u);
  EXPECT_EQ(ABI.ImageReads[0].Begin, F.DataVA);
  EXPECT_EQ(ABI.ImageWrites[0].End, F.DataVA + 4);
  for (unsigned Index = 0; Index != 3; ++Index)
    EXPECT_TRUE(llvm::any_of(ABI.CodeRanges, [&](const auto &Range) {
      return Range.contains(F.address(Index));
    }));
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(
      F.Image, F.Image.Entry, nullptr, RegistrationLeafResult::Discarded));
  EXPECT_FALSE(
      getCheckedX86RegistrationCopyCalleeABI(F.Image, F.Image.Entry, 12));
}

TEST(RegistrationObjectGraph, TranslatesSubobjectEffectsAndChecksExtent) {
  ObjectGraphImage F;
  F.install(0, F.wrapper(0, {1}, {0x83, 0xc1, 4}));
  F.leaf(1);
  auto Proof = F.prove();
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->Destructor->ECXReads,
            (std::vector<RegistrationObjectExtent>{{4, 8}}));
  EXPECT_EQ(Proof->Destructor->ECXWrites, Proof->Destructor->ECXReads);
  for (uint8_t Offset : {uint8_t(0xfc), uint8_t(12)}) {
    F.install(0, F.wrapper(0, {1}, {0x83, 0xc1, Offset}));
    EXPECT_FALSE(F.prove());
  }
}

TEST(RegistrationObjectGraph, RejectsOpaqueCallsAndBrokenFrameContracts) {
  const std::pair<const char *, std::vector<uint8_t>> Children[] = {
      {"object escape", {0x89, 0x0d, 0, 0, 9, 0, 0xc3}},
      {"caller stack", {0x8b, 0x44, 0x24, 4, 0xc3}},
      {"runtime return PC", {0x8b, 4, 0x24, 0xa3, 0, 0, 9, 0, 0xc3}},
      {"nonvolatile clobber", {0x31, 0xdb, 0xc3}},
      {"indirect call", {0xff, 0xd0, 0xc3}},
      {"FS chain", {0x64, 0xa1, 0, 0, 0, 0, 0xc3}},
      {"metadata write", {0xc7, 0x05, 0, 0, 8, 0, 0, 0, 0, 0, 0xc3}},
      {"code write", {0xc7, 0x05, 0, 0x12, 0, 0, 0, 0, 0, 0, 0xc3}}};
  for (const auto &[Name, Child] : Children) {
    SCOPED_TRACE(Name);
    ObjectGraphImage F;
    F.install(0, F.wrapper(0, {1}));
    F.install(1, Child);
    EXPECT_FALSE(F.prove());
  }
  for (const auto &Prefix :
       {std::vector<uint8_t>{0x89, 0xe1}, std::vector<uint8_t>{0x31, 0xc9}}) {
    ObjectGraphImage F;
    F.install(0, F.wrapper(0, {1}, Prefix));
    F.leaf(1);
    EXPECT_FALSE(F.prove());
  }
}

TEST(RegistrationObjectGraph, DoesNotTreatDiscardedChildResultAsAValue) {
  ObjectGraphImage F;
  F.leaf(1);
  // Neither a borrowed pointer nor an unproved scalar return can be stored.
  for (const auto &Suffix : {std::vector<uint8_t>{0xa3, 0, 0, 9, 0},
                             std::vector<uint8_t>{0x89, 0x06},
                             std::vector<uint8_t>{0x85, 0xc0, 0x74, 0, 0x90}}) {
    F.install(0, F.wrapper(0, {1}, {}, Suffix));
    EXPECT_FALSE(F.prove());
  }
}

TEST(RegistrationObjectGraph, ObservedRootNeedsAnIndependentScalarResult) {
  ObjectGraphImage F;
  F.leaf(1);
  F.install(0, F.wrapper(0, {1}));
  ASSERT_TRUE(F.prove());
  EXPECT_FALSE(
      getCheckedX86RegistrationObjectCalleeABI(F.Image, F.Image.Entry));
  F.install(0, F.wrapper(0, {1}, {}, {0x31, 0xc0}));
  const auto ABI =
      getCheckedX86RegistrationObjectCalleeABI(F.Image, F.Image.Entry);
  ASSERT_TRUE(ABI);
  EXPECT_TRUE(ABI->HasIndependentScalarReturn);
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(F.Image, F.Image.Entry));
}

TEST(RegistrationObjectGraph, BindsPrivateScalarArgumentsAndExactCleanup) {
  ObjectGraphImage F;
  F.install(0, F.wrapper(0, {1}, {0x6a, 7}));
  F.install(1, {0x8b, 0x44, 0x24, 4, 0x89, 0x01, 0xc2, 4, 0});
  const auto Proof = F.prove();
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->Destructor->StackPopBytes, 0u);
  EXPECT_EQ(Proof->Destructor->ECXWrites,
            (std::vector<RegistrationObjectExtent>{{0, 4}}));
  // Extra cleanup consumes saved ESI; an extra read observes the caller frame.
  F.install(1, {0x8b, 0x44, 0x24, 4, 0x89, 0x01, 0xc2, 8, 0});
  EXPECT_FALSE(F.prove());
  F.install(1, {0x8b, 0x44, 0x24, 8, 0x89, 0x01, 0xc2, 4, 0});
  EXPECT_FALSE(F.prove());
  F.install(1, {0x8b, 0x44, 0x24, 4, 0x89, 0x01, 0xc2, 4, 0});
  for (const auto &Argument :
       {std::vector<uint8_t>{0x51}, std::vector<uint8_t>{0x54},
        std::vector<uint8_t>{0x83, 0xec, 4}}) {
    F.install(0, F.wrapper(0, {1}, Argument));
    EXPECT_FALSE(F.prove());
  }
}

TEST(RegistrationObjectGraph, DoesNotExposeUnboundRootScalarArguments) {
  ObjectGraphImage F;
  F.install(0, {0x8b, 0x44, 0x24, 4, 0x89, 0x01, 0xc2, 4, 0});
  // Scalar arguments are admissible only at a child occurrence whose private
  // outgoing words were proved. Ordinary source contracts currently bind only
  // a zero-pop root; its caller cannot inherit the child's argument authority.
  EXPECT_FALSE(
      getCheckedX86RegistrationObjectCalleeABI(F.Image, F.Image.Entry));
  EXPECT_FALSE(getCheckedX86RegistrationObjectCalleeABI(
      F.Image, F.Image.Entry, nullptr, RegistrationLeafResult::Discarded));
}

TEST(RegistrationObjectGraph, RetainsMayEffectsAcrossConditionalChildCalls) {
  ObjectGraphImage F;
  // An independently loaded scalar controls whether this invocation calls
  // its child. No child writes become constructor initialization evidence.
  F.install(0, F.wrapper(0, {1}, {0xa1, 0, 0, 9, 0, 0x85, 0xc0, 0x74, 5}));
  F.leaf(1);
  const auto Proof = F.prove();
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->Destructor->ECXWrites,
            (std::vector<RegistrationObjectExtent>{{0, 4}}));
  EXPECT_FALSE(
      getCheckedX86RegistrationInitializerCalleeABI(F.Image, F.Image.Entry));
}

TEST(RegistrationObjectGraph, RejectsRecursionDepthAndWorkExhaustion) {
  for (bool Mutual : {false, true}) {
    ObjectGraphImage F;
    F.install(0, F.wrapper(0, {Mutual ? 1u : 0u}));
    F.install(1, F.wrapper(1, {0}));
    EXPECT_FALSE(F.prove());
  }
  ObjectGraphImage F(40);
  for (unsigned Index = 0; Index != 39; ++Index)
    F.install(Index, F.wrapper(Index, {Index + 1}));
  F.leaf(39);
  EXPECT_FALSE(F.prove());
  F.leaf(1);
  ASSERT_TRUE(F.prove());
  size_t Work = limits::kMaxRegistrationEHStateWork - 1;
  EXPECT_FALSE(F.prove(&Work));
  EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationObjectGraph, BoundsTheNumberOfDistinctChildren) {
  ObjectGraphImage F(300);
  std::vector<unsigned> Targets;
  for (unsigned Index = 1; Index != 21; ++Index) {
    Targets.push_back(Index);
    std::vector<unsigned> Leaves;
    for (unsigned J = 0; J != 13; ++J) {
      const unsigned Leaf = 21 + (Index - 1) * 13 + J;
      Leaves.push_back(Leaf);
      F.leaf(Leaf);
    }
    F.install(Index, F.wrapper(Index, Leaves));
  }
  F.install(0, F.wrapper(0, Targets));
  EXPECT_FALSE(F.prove());
  Targets.resize(2);
  F.install(0, F.wrapper(0, Targets));
  EXPECT_TRUE(F.prove());
}

TEST(RegistrationObjectGraph, RechecksChangedTransitiveCodeAndTargets) {
  ObjectGraphImage F;
  F.install(0, F.wrapper(0, {1}));
  F.leaf(1);
  ASSERT_TRUE(F.prove());
  F.install(1, {0x31, 0xdb, 0xc3});
  EXPECT_FALSE(F.prove());
  F.leaf(1);
  ASSERT_TRUE(F.prove());
  F.install(0, F.wrapper(0, {2}));
  EXPECT_FALSE(F.prove());
  F.install(0, F.wrapper(0, {1}));
  F.Image.Segments[0].Flags =
      F.Image.Segments[0].Flags | SegmentFlags::Writable;
  EXPECT_FALSE(F.prove());
}

TEST(RegistrationObjectGraph, InputPE32BindsTransitiveDestruction) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Entry = llvm::find_if(Image->Exports, [](const auto &Export) {
    return Export.Name == "callback_parent";
  });
  ASSERT_NE(Entry, Image->Exports.end());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Low = CFGBuilder().build(*Image, Decode, Entry->Addr);
  ASSERT_TRUE(Low.RegistrationStates);
  const auto &States = *Low.RegistrationStates;
  ASSERT_TRUE(States.Complete);
  ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
  ASSERT_TRUE(States.CleanupFrameEffectsComplete);
  std::set<va_t> Destructors;
  for (const auto &Contract : States.CalleeContracts) {
    std::vector<va_t> Tables;
    if (Contract.CalleeKind ==
        RegistrationCalleeFrameContract::Kind::PrivateThrow) {
      const auto Throw =
          getCheckedX86RegistrationThrowCalleeABI(*Image, Contract.Target);
      ASSERT_TRUE(Throw);
      Tables.push_back(Throw->ThrowInfo.Address);
    }
    for (const auto &Info : Contract.RuntimeThrowInfos)
      Tables.push_back(Info.Address);
    for (va_t Table : Tables) {
      const auto Object =
          getCheckedX86RegistrationThrowObjectABI(*Image, Table);
      ASSERT_TRUE(Object);
      if (!Object->Destructor)
        continue;
      const auto &Destructor = *Object->Destructor;
      Destructors.insert(Destructor.Target);
      EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(
          *Image, Destructor.Target, nullptr,
          RegistrationLeafResult::Discarded));
      unsigned ChildEntries = 0;
      const auto Body = CFGBuilder().build(*Image, Decode, Destructor.Target);
      const auto EntryBlock =
          llvm::find_if(Body.Blocks, [&](const auto &Block) {
            return Block.StartAddr == Body.Entry;
          });
      ASSERT_NE(EntryBlock, Body.Blocks.end());
      std::vector<int> Pending{EntryBlock->Id};
      std::set<int> Reached;
      while (!Pending.empty()) {
        const int Id = Pending.back();
        Pending.pop_back();
        const auto Block =
            llvm::find_if(Body.Blocks, [&](const auto &Candidate) {
              return Candidate.Id == Id;
            });
        ASSERT_NE(Block, Body.Blocks.end());
        if (!Reached.insert(Id).second)
          continue;
        Pending.insert(Pending.end(), Block->Succs.begin(), Block->Succs.end());
      }
      // A full lift can retain independently addressed roots. Only paths from
      // this destructor's actual entry belong to the current invocation.
      for (const auto &Block : Body.Blocks) {
        if (!Reached.count(Block.Id))
          continue;
        for (const auto &Op : Block.Ops) {
          if (Op.Opcode != NdOp::CALL)
            continue;
          ASSERT_EQ(Op.NumInputs, 1u);
          ASSERT_TRUE(Op.Inputs[0].isConst());
          EXPECT_NE(Op.Inputs[0].Offset, Destructor.Target);
          EXPECT_TRUE(
              llvm::any_of(Destructor.CodeRanges, [&](const auto &Range) {
                return Range.contains(Op.Inputs[0].Offset);
              }));
          ++ChildEntries;
        }
      }
      EXPECT_EQ(ChildEntries, 2u);
      for (const auto &Range : Destructor.CodeRanges)
        EXPECT_TRUE(llvm::any_of(Contract.CodeRanges, [&](const auto &Owner) {
          return Owner.Begin <= Range.Begin && Range.End <= Owner.End;
        }));
    }
  }
  EXPECT_EQ(Destructors.size(), 2u);
}

} // namespace
