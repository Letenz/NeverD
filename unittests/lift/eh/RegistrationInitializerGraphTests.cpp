//===- RegistrationInitializerGraphTests.cpp - PE32 constructor graphs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose ordinary constructors without mixing object and private-frame bytes.
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

#include <algorithm>
#include <cstdlib>
#include <set>

namespace {
using namespace neverd;

struct ConstructorImage {
  BinaryImage Image;

  explicit ConstructorImage(unsigned Count = 3) {
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
    Segment Data;
    Data.VA = 0x8000;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(16);
    Data.Size = Data.FileSz = Data.Data.size();
    Image.Segments.push_back(std::move(Data));
    for (unsigned I = 0; I != Count; ++I)
      Image.Symbols.push_back(Symbol::makeFunc(address(I)));
  }

  static va_t address(unsigned Index) { return 0x1000 + Index * 0x100; }

  void install(unsigned Index, llvm::ArrayRef<uint8_t> Code) {
    ASSERT_LE(Code.size(), 0x100u);
    ASSERT_LT(Index * 0x100, Image.Segments[0].Data.size());
    llvm::copy(Code, Image.Segments[0].Data.begin() + Index * 0x100);
  }

  void leaf(unsigned Index, bool Partial = false) {
    std::vector<uint8_t> Code = {0x8b, 0x44, 0x24, 4,    0x89, 1,    0x8b, 0x44,
                                 0x24, 8,    0x89, 0x41, 4,    0xff, 0x05, 0,
                                 0x80, 0,    0,    0x89, 0xc8, 0xc2, 8,    0};
    if (Partial)
      std::fill(Code.begin() + 10, Code.begin() + 13, 0x90);
    install(Index, Code);
  }

  std::vector<uint8_t> wrapper(unsigned Index, unsigned Target,
                               llvm::ArrayRef<uint8_t> BeforeCall = {},
                               llvm::ArrayRef<uint8_t> AfterCall = {}) {
    // ESI preserves the original object across the nested thiscall. Private
    // outgoing scalar words remain below both nonvolatile-register saves.
    std::vector<uint8_t> Code = {0x55, 0x89, 0xe5, 0x56, 0x89, 0xce, 0x83, 0xec,
                                 8,    0x8b, 0x45, 8,    0x89, 4,    0x24, 0x8b,
                                 0x45, 12,   0x89, 0x44, 0x24, 4};
    Code.insert(Code.end(), BeforeCall.begin(), BeforeCall.end());
    Code.push_back(0xe8);
    const size_t Offset = Code.size();
    Code.resize(Offset + 4);
    writeLE<uint32_t>(
        Code.data() + Offset,
        uint32_t(address(Target) - (address(Index) + Code.size())));
    Code.insert(Code.end(), AfterCall.begin(), AfterCall.end());
    const uint8_t Return[] = {0x89, 0xf0, 0x5e, 0x5d, 0xc2, 8, 0};
    Code.insert(Code.end(), std::begin(Return), std::end(Return));
    return Code;
  }

  std::optional<RegistrationInitializerCalleeABI> prove() const {
    return getCheckedX86RegistrationInitializerCalleeABI(Image, Image.Entry);
  }
};

TEST(RegistrationInitializerGraph, CarriesTransitiveObjectAndImageEffects) {
  ConstructorImage F;
  F.install(0, F.wrapper(0, 1));
  F.install(1, F.wrapper(1, 2));
  F.leaf(2);
  const auto ABI = F.prove();
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->Object.StackPopBytes, 8u);
  EXPECT_FALSE(ABI->Object.HasIndependentScalarReturn);
  EXPECT_EQ(ABI->Initialized, (std::vector<RegistrationObjectExtent>{{0, 8}}));
  EXPECT_EQ(ABI->Object.ECXWrites, ABI->Initialized);
  ASSERT_EQ(ABI->Object.ImageWrites.size(), 1u);
  EXPECT_EQ(ABI->Object.ImageWrites[0].Begin, 0x8000u);
  EXPECT_EQ(ABI->Object.ImageWrites[0].End, 0x8004u);
  for (unsigned I = 0; I != 3; ++I)
    EXPECT_TRUE(llvm::any_of(ABI->Object.CodeRanges, [&](const auto &Range) {
      return Range.contains(F.address(I));
    }));
  // A constructor graph cannot authorize the generic leaf or copy ABI.
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(F.Image, F.Image.Entry));
  EXPECT_FALSE(
      getCheckedX86RegistrationCopyCalleeABI(F.Image, F.Image.Entry, 8));
}

TEST(RegistrationInitializerGraph, TranslatesSubobjectWritesAndKeepsThis) {
  ConstructorImage F;
  F.install(0, F.wrapper(0, 1, {0x83, 0xc1, 4}));
  F.leaf(1);
  const auto ABI = F.prove();
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->Initialized, (std::vector<RegistrationObjectExtent>{{4, 12}}));
  EXPECT_EQ(ABI->Object.ECXWrites, ABI->Initialized);
  auto Changed = F.wrapper(0, 1, {0x83, 0xc1, 4});
  // Return the child's subobject rather than the original object.
  std::fill(Changed.end() - 7, Changed.end() - 5, 0x90);
  F.install(0, Changed);
  EXPECT_FALSE(F.prove());
}

TEST(RegistrationInitializerGraph, ReusesOneChildForIndependentSubobjects) {
  ConstructorImage F;
  auto Code = F.wrapper(0, 1);
  Code.resize(Code.size() - 7);
  const uint8_t Arguments[] = {0x83, 0xec, 8,    0x89, 0xf1, 0x83, 0xc1,
                               8,    0x8b, 0x45, 8,    0x89, 4,    0x24,
                               0x8b, 0x45, 12,   0x89, 0x44, 0x24, 4};
  Code.insert(Code.end(), std::begin(Arguments), std::end(Arguments));
  Code.push_back(0xe8);
  const size_t Offset = Code.size();
  Code.resize(Offset + 4);
  writeLE<uint32_t>(Code.data() + Offset,
                    uint32_t(F.address(1) - (F.address(0) + Code.size())));
  const uint8_t Return[] = {0x89, 0xf0, 0x5e, 0x5d, 0xc2, 8, 0};
  Code.insert(Code.end(), std::begin(Return), std::end(Return));
  F.install(0, Code);
  F.leaf(1);
  const auto ABI = F.prove();
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->Initialized, (std::vector<RegistrationObjectExtent>{{0, 16}}));
  EXPECT_EQ(ABI->Object.ECXWrites, ABI->Initialized);
  EXPECT_EQ(llvm::count_if(ABI->Object.CodeRanges,
                           [&](const auto &Range) {
                             return Range.contains(F.address(1));
                           }),
            1);
}

TEST(RegistrationInitializerGraph, RetainsOnlyWritesCompletedByTheChild) {
  ConstructorImage F;
  F.install(0, F.wrapper(0, 1));
  F.leaf(1, true);
  auto ABI = F.prove();
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->Initialized, (std::vector<RegistrationObjectExtent>{{0, 4}}));
  // Reading the child's missing second word cannot grant initialization.
  F.install(0, F.wrapper(0, 1, {}, {0x8b, 0x46, 4}));
  EXPECT_FALSE(F.prove());
}

TEST(RegistrationInitializerGraph,
     RejectsPointerArgumentsAndWrongObjectDomains) {
  const std::pair<const char *, std::vector<uint8_t>> Cases[] = {
      {"borrowed object argument", {0x89, 0x0c, 0x24}},
      {"partial object pointer argument", {0x89, 0x0c, 0x24, 0xc6, 4, 0x24, 0}},
      {"private frame argument", {0x89, 0x24, 0x24}},
      {"private destination", {0x89, 0xe1}},
      {"unknown destination", {0x8b, 0x4d, 8}},
      {"negative subobject", {0x83, 0xe9, 4}},
      {"released argument storage", {0x83, 0xc4, 8}}};
  for (const auto &[Name, Prefix] : Cases) {
    SCOPED_TRACE(Name);
    ConstructorImage F;
    F.install(0, F.wrapper(0, 1, Prefix));
    F.leaf(1);
    EXPECT_FALSE(F.prove());
  }
  ConstructorImage F;
  auto Code = F.wrapper(0, 1);
  std::fill(Code.begin() + 18, Code.begin() + 22, 0x90);
  F.install(0, Code);
  F.leaf(1);
  EXPECT_FALSE(F.prove());
  F.install(0, F.wrapper(0, 1));
  // This child consumes twelve bytes: the third word would be the caller's
  // saved ESI, rather than a private initialized scalar argument.
  auto Child = std::vector<uint8_t>{0x8b, 0x44, 0x24, 4,  0x89, 1,
                                    0x89, 0xc8, 0xc2, 12, 0};
  F.install(1, Child);
  EXPECT_FALSE(F.prove());
}

TEST(RegistrationInitializerGraph, RejectsRecursionAndExhaustedBudgets) {
  for (bool Mutual : {false, true}) {
    ConstructorImage F;
    F.install(0, F.wrapper(0, Mutual ? 1 : 0));
    F.install(1, F.wrapper(1, 0));
    EXPECT_FALSE(F.prove());
  }
  ConstructorImage F(40);
  for (unsigned I = 0; I != 39; ++I)
    F.install(I, F.wrapper(I, I + 1));
  F.leaf(39);
  EXPECT_FALSE(F.prove());
  F.leaf(1);
  ASSERT_TRUE(F.prove());
  size_t Work = limits::kMaxRegistrationEHStateWork - 1;
  EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
      F.Image, F.Image.Entry, &Work));
  EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationInitializerGraph, InputPE32BindsTransitiveConstruction) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  std::set<uint32_t> RootPops, ChildPops;
  for (const auto &Export : Image->Exports) {
    if (Export.Name != "callback_throw_value" &&
        Export.Name != "callback_throw_reference")
      continue;
    const auto Throw =
        getCheckedX86RegistrationThrowCalleeABI(*Image, Export.Addr);
    ASSERT_TRUE(Throw) << Export.Name;
    const auto Low = CFGBuilder().build(*Image, Decode, Export.Addr);
    unsigned Graphs = 0;
    for (const auto &Block : Low.Blocks)
      for (const auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
            !Op.Inputs[0].isConst())
          continue;
        const auto Root = getCheckedX86RegistrationInitializerCalleeABI(
            *Image, Op.Inputs[0].Offset);
        if (!Root)
          continue;
        ++Graphs;
        RootPops.insert(Root->Object.StackPopBytes);
        const std::vector<RegistrationObjectExtent> Initialized = {
            {0, int32_t(Throw->ThrowInfo.ObjectSize)}};
        EXPECT_EQ(Root->Initialized, Initialized);
        const auto Body =
            CFGBuilder().build(*Image, Decode, Root->Object.Target);
        // The full lift can retain address-taken entries. Only ordinary paths
        // from this constructor belong to this particular invocation.
        const auto Entry = llvm::find_if(Body.Blocks, [&](const auto &B) {
          return B.StartAddr == Body.Entry;
        });
        ASSERT_NE(Entry, Body.Blocks.end());
        std::vector<int> Pending = {Entry->Id};
        std::set<int> Reached;
        while (!Pending.empty()) {
          const int Id = Pending.back();
          Pending.pop_back();
          const auto Block = llvm::find_if(
              Body.Blocks, [&](const auto &B) { return B.Id == Id; });
          ASSERT_NE(Block, Body.Blocks.end());
          if (!Reached.insert(Id).second)
            continue;
          Pending.insert(Pending.end(), Block->Succs.begin(),
                         Block->Succs.end());
        }
        unsigned Children = 0;
        for (const auto &ChildBlock : Body.Blocks) {
          if (!Reached.count(ChildBlock.Id))
            continue;
          for (const auto &Call : ChildBlock.Ops) {
            if (Call.Opcode != NdOp::CALL)
              continue;
            ASSERT_EQ(Call.NumInputs, 1u);
            ASSERT_TRUE(Call.Inputs[0].isConst());
            const auto Child = getCheckedX86RegistrationInitializerCalleeABI(
                *Image, Call.Inputs[0].Offset);
            ASSERT_TRUE(Child) << Root->Object.Target << ": " << Call.Addr;
            EXPECT_NE(Child->Object.Target, Root->Object.Target);
            EXPECT_EQ(Child->Initialized, Initialized);
            ChildPops.insert(Child->Object.StackPopBytes);
            for (const auto &Range : Child->Object.CodeRanges)
              EXPECT_TRUE(
                  llvm::any_of(Root->Object.CodeRanges, [&](const auto &Owner) {
                    return Owner.Begin <= Range.Begin && Range.End <= Owner.End;
                  }));
            ++Children;
          }
        }
        EXPECT_EQ(Children, 1u);
        for (const auto &Range : Root->Object.CodeRanges)
          EXPECT_TRUE(llvm::any_of(Throw->CodeRanges, [&](const auto &Owner) {
            return Owner.Begin <= Range.Begin && Range.End <= Owner.End;
          }));
      }
    EXPECT_EQ(Graphs, 1u);
  }
  EXPECT_EQ(RootPops, (std::set<uint32_t>{8, 12}));
  EXPECT_EQ(ChildPops, (std::set<uint32_t>{12, 16}));
}

} // namespace
