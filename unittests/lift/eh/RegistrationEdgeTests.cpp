//===- RegistrationEdgeTests.cpp - Shared C++ exception edges -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/support/BinaryEncoding.h"

#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {
using namespace neverd;
constexpr va_t Entry = 0x401000;

BinaryImage cleanupImage(unsigned States, unsigned BodyBlocks,
                         bool AliasedActions = false) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  auto &Bytes = Text.Data;
  const auto Word = [&](uint32_t Value) {
    for (unsigned I = 0; I != 4; ++I)
      Bytes.push_back(uint8_t(Value >> (8 * I)));
  };
  const auto Address = [&] { return Entry + Bytes.size(); };
  Bytes = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
  Word(0x490000); // push personality
  Bytes.insert(Bytes.end(), {0x64, 0xa1, 0, 0, 0, 0, 0x50});
  const va_t Install = Address();
  Bytes.insert(Bytes.end(), {0x64, 0x89, 0x25, 0, 0, 0, 0});
  const va_t Store = Address();
  Bytes.insert(Bytes.end(), {0xc7, 0x45, 0xfc, 0, 0, 0, 0, 0xeb, 0});
  // The saved chain pointer in EAX is not a proved try level. This store
  // invalidates the preceding known level; ordinary branches carry that
  // unknown state into independently decoded intervals.
  Bytes.insert(Bytes.end(), {0x89, 0x45, 0xfc, 0xeb, 0});
  for (unsigned I = 0; I != BodyBlocks; ++I)
    Bytes.insert(Bytes.end(), {0x90, 0xeb, 0});
  Bytes.insert(Bytes.end(), {0x8b, 0x4d, 0xf4, 0x64, 0x89, 0x0d, 0, 0, 0, 0,
                             0x8b, 0xe5, 0x5d, 0xc3});
  const va_t FirstCleanup = Address();
  Bytes.insert(Bytes.end(), {0xb8, 1, 0, 0, 0, 0xc3});
  const va_t SecondCleanup = Address();
  Bytes.insert(Bytes.end(), {0xb8, 2, 0, 0, 0, 0xc3});
  const va_t End = Address();
  Text.Size = Text.FileSz = Bytes.size();
  Image.Segments.push_back(std::move(Text));
  Segment Personality;
  Personality.VA = 0x490000;
  Personality.Size = Personality.FileSz = 1;
  Personality.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Personality.Data = {0xc3};
  Image.Segments.push_back(std::move(Personality));
  Image.Symbols.push_back(Symbol::makeFunc(Entry, End - Entry));
  ExceptionFunction EH;
  EH.CodeRange = {Entry, End};
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  auto &Chain = EH.Registration.emplace();
  Chain.RegistrationOffset = -12;
  Chain.TryLevelOffset = -4;
  Chain.SeededTryLevel = -1;
  Chain.ChainInstallVA = Install;
  Chain.TryLevelStores = {{Store, Store + 7, 0}};
  auto &Cxx = EH.Cxx.emplace();
  Cxx.Magic = 0x19930522;
  Cxx.MaxState = States;
  for (unsigned I = 0; I != States; ++I)
    Cxx.UnwindMap.push_back(
        {int32_t(I) - 1, 0, CxxUnwindAction::ActionKind::None});
  Cxx.UnwindMap[0] = {-1, FirstCleanup, CxxUnwindAction::ActionKind::Direct};
  Cxx.UnwindMap[States / 2].ActionVA =
      AliasedActions ? FirstCleanup : SecondCleanup;
  Cxx.UnwindMap[States / 2].Kind = CxxUnwindAction::ActionKind::Direct;
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();
  return Image;
}

LowFunc build(const BinaryImage &Image) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Image));
  return CFGBuilder().build(Image, Dec, Entry, "shared_cleanup_contexts");
}

using FlatEdge = std::tuple<va_t, ExceptionalEdgeKind, uint32_t, int32_t>;
std::set<FlatEdge> flatten(const std::vector<ExceptionalEdge> &Edges) {
  std::set<FlatEdge> Result;
  for (const auto &Edge : Edges)
    for (const auto &Context : Edge.contexts())
      Result.emplace(Edge.TargetVA, Edge.Kind, Context.RegionIndex,
                     Context.State);
  return Result;
}

TEST(RegistrationEdges, LongEmptyChainsShareEveryOriginAndAction) {
  for (unsigned States : {9u, 257u, 4096u})
    for (unsigned Body : {0u, 256u, 2048u})
      for (bool Alias : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << States << '/' << Body << '/' << Alias);
        auto Image = cleanupImage(States, Body, Alias);
        auto F = build(Image);
        ASSERT_TRUE(F.hasCompleteLiftCoverage());
        ASSERT_TRUE(F.RegistrationStates);
        const auto &Analysis = *F.RegistrationStates;
        ASSERT_EQ(Analysis.Blocks.size(), F.Blocks.size());
        EXPECT_FALSE(Analysis.Complete);
        EXPECT_FALSE(Analysis.RegistrationLifetimeComplete);
        EXPECT_FALSE(Analysis.CleanupFrameEffectsComplete);
        EXPECT_TRUE(Analysis.CleanupFrameEffects.empty());
        EXPECT_TRUE(std::none_of(Analysis.Diagnostics.begin(),
                                 Analysis.Diagnostics.end(), [](const auto &D) {
                                   return D.find("budget exhausted") !=
                                          std::string::npos;
                                 }));
        const auto &Cxx = *F.ExceptionMetadata->Cxx;
        // Independent closed-form oracle for the two nonempty actions in
        // this chain. Aliased targets still retain two distinct regions.
        std::set<FlatEdge> Expected;
        for (unsigned State = 0; State != States; ++State) {
          Expected.emplace(Cxx.UnwindMap[0].ActionVA,
                           ExceptionalEdgeKind::CxxCleanup, 0, State);
          if (State >= States / 2)
            Expected.emplace(Cxx.UnwindMap[States / 2].ActionVA,
                             ExceptionalEdgeKind::CxxCleanup, States / 2,
                             State);
        }
        std::vector<ExceptionalEdge> Pattern;
        size_t UnknownSources = 0;
        std::map<int, std::vector<ExceptionalEdge>> Predecessors;
        for (const auto &Block : F.Blocks) {
          const auto Facts = std::find_if(
              Analysis.Blocks.begin(), Analysis.Blocks.end(),
              [&](const auto &S) { return S.BlockId == Block.Id; });
          ASSERT_NE(Facts, Analysis.Blocks.end());
          if (Facts->CanDispatch && Facts->Unknown) {
            ++UnknownSources;
            if (Pattern.empty()) {
              Pattern = Block.ExceptionalSuccs;
              EXPECT_EQ(flatten(Pattern), Expected);
            }
            ASSERT_EQ(Block.ExceptionalSuccs.size(), Alias ? 1u : 2u);
            for (size_t I = 0; I != Pattern.size(); ++I) {
              EXPECT_EQ(Block.ExceptionalSuccs[I], Pattern[I]);
              EXPECT_EQ(Block.ExceptionalSuccs[I].SharedContexts,
                        Pattern[I].SharedContexts);
              EXPECT_TRUE(Pattern[I].SharedContexts);
            }
          }
          for (auto Edge : Block.ExceptionalSuccs) {
            const int Target = Edge.BlockId;
            if (Target >= 0) {
              Edge.BlockId = Block.Id;
              Predecessors[Target].push_back(std::move(Edge));
            }
          }
        }
        EXPECT_GE(UnknownSources, Body + 1);
        for (const auto &Block : F.Blocks)
          EXPECT_EQ(Block.ExceptionalPreds, Predecessors[Block.Id]);
      }
}

TEST(RegistrationEdges, GroupingPreservesAnIrregularGraphAndCatchSearches) {
  auto Image = cleanupImage(9, 8);
  auto &Cxx = *Image.ExceptionMetadata.Functions.front().Cxx;
  const va_t First = Cxx.UnwindMap[0].ActionVA;
  const va_t Second = Cxx.UnwindMap[4].ActionVA;
  const int32_t Parents[] = {-1, 0, 0, 1, 3, 2, 5, 4, 6};
  for (unsigned I = 0; I != 9; ++I) {
    Cxx.UnwindMap[I].ToState = Parents[I];
    Cxx.UnwindMap[I].ActionVA = I % 3 == 0 ? 0 : I % 2 ? First : Second;
    Cxx.UnwindMap[I].Kind = Cxx.UnwindMap[I].ActionVA
                                ? CxxUnwindAction::ActionKind::Direct
                                : CxxUnwindAction::ActionKind::None;
  }
  CxxTryBlock Try;
  Try.TryLow = 1;
  Try.TryHigh = 3;
  Try.CatchHigh = 4;
  CxxCatchHandler Catch;
  auto &Text = Image.Segments.front();
  Catch.HandlerVA = Text.VA + Text.Data.size();
  Text.Data.insert(Text.Data.end(), {0xb8, 0, 0, 0, 0, 0xc3});
  Text.Size = Text.FileSz = Text.Data.size();
  Image.ExceptionMetadata.Functions.front().CodeRange.End = Text.VA + Text.Size;
  Image.Symbols.front() = Symbol::makeFunc(Entry, Text.Size);
  Try.Handlers = {Catch, Catch};
  Cxx.TryBlocks.push_back(Try);
  Image.ExceptionMetadata.rebuildIndex();
  auto F = build(Image);
  ASSERT_TRUE(F.RegistrationStates);
  ASSERT_EQ(F.RegistrationStates->Blocks.size(), F.Blocks.size());
  size_t CatchEdges = 0;
  for (const auto &Block : F.Blocks) {
    const auto State =
        std::find_if(F.RegistrationStates->Blocks.begin(),
                     F.RegistrationStates->Blocks.end(),
                     [&](const auto &S) { return S.BlockId == Block.Id; });
    ASSERT_NE(State, F.RegistrationStates->Blocks.end());
    std::set<FlatEdge> Expected;
    if (State->CanDispatch)
      for (int32_t Origin : State->levels()) {
        for (int32_t Walk = Origin; Walk >= 0;
             Walk = Cxx.UnwindMap[Walk].ToState)
          if (Cxx.UnwindMap[Walk].ActionVA)
            Expected.emplace(Cxx.UnwindMap[Walk].ActionVA,
                             ExceptionalEdgeKind::CxxCleanup, Walk, Origin);
        for (const auto &Search : State->CxxSearches)
          if (Search.Level == Origin)
            for (const auto &Handler : Cxx.TryBlocks[Search.TryIndex].Handlers)
              Expected.emplace(Handler.HandlerVA, ExceptionalEdgeKind::CxxCatch,
                               Search.TryIndex, Origin);
      }
    EXPECT_EQ(flatten(Block.ExceptionalSuccs), Expected);
    for (const auto &Edge : Block.ExceptionalSuccs)
      CatchEdges += Edge.Kind == ExceptionalEdgeKind::CxxCatch;
  }
  EXPECT_GT(CatchEdges, 0u);
}

TEST(RegistrationEdges, DenseContextsStillExhaustTheUnchangedWorkBudget) {
  auto Image = cleanupImage(limits::kMaxRegistrationEHRecords, 0, true);
  auto &Map = Image.ExceptionMetadata.Functions.front().Cxx->UnwindMap;
  for (auto &Action : Map) {
    Action.ActionVA = Map[0].ActionVA;
    Action.Kind = CxxUnwindAction::ActionKind::Direct;
  }
  auto F = build(Image);
  ASSERT_TRUE(F.RegistrationStates);
  EXPECT_FALSE(F.RegistrationStates->Complete);
  EXPECT_TRUE(F.RegistrationStates->Blocks.empty());
  EXPECT_NE(
      std::find(F.RegistrationStates->Diagnostics.begin(),
                F.RegistrationStates->Diagnostics.end(),
                "registration exceptional-edge expansion budget exhausted"),
      F.RegistrationStates->Diagnostics.end());
  for (const auto &Block : F.Blocks) {
    EXPECT_TRUE(Block.ExceptionalSuccs.empty());
    EXPECT_TRUE(Block.ExceptionalPreds.empty());
  }
}

TEST(RegistrationEdges, InvalidStateGraphsCannotPublishPartialEdges) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = cleanupImage(9, 4);
    auto &Cxx = *Image.ExceptionMetadata.Functions.front().Cxx;
    if (Mutation < 3)
      Cxx.UnwindMap[3].ToState = Mutation == 0 ? 3 : Mutation == 1 ? 4 : -2;
    else
      ++Cxx.MaxState;
    auto F = build(Image);
    ASSERT_TRUE(F.RegistrationStates);
    EXPECT_FALSE(F.RegistrationStates->Complete);
    EXPECT_TRUE(F.RegistrationStates->Blocks.empty());
    for (const auto &Block : F.Blocks)
      EXPECT_TRUE(Block.ExceptionalSuccs.empty());
  }
}

TEST(RegistrationEdges, OwnedDomainsBindExactPairsAcrossIndependentLifetimes) {
  const std::vector<ExceptionalEdgeContext> Values{{0, 3}, {1, 4}};
  auto Input = Values;
  const auto InputStorage = reinterpret_cast<uintptr_t>(Input.data());
  ExceptionalEdge A(1, 0x2000, ExceptionalEdgeKind::CxxCleanup);
  A.SharedContexts =
      std::make_shared<const ExceptionalEdgeContextDomain>(std::move(Input));
  // The immutable digest may not cache a buffer a producer could still reach
  // through references taken before moving its vector into the constructor.
  EXPECT_NE(reinterpret_cast<uintptr_t>(A.contexts().data()), InputStorage);
  ExceptionalEdge B = A;
  B.SharedContexts = std::make_shared<const ExceptionalEdgeContextDomain>(
      std::vector<ExceptionalEdgeContext>{{1, 4}, {0, 3}, {1, 4}});
  EXPECT_EQ(A, B);
  EXPECT_EQ(A.SharedContexts->digest(), B.SharedContexts->digest());
  EXPECT_FALSE(A < B);
  EXPECT_FALSE(B < A);
  EXPECT_EQ(A.singleContext(), nullptr);
  LowFunc F;
  F.Blocks.emplace_back();
  F.Blocks.front().ExceptionalSuccs = {A};
  const auto OriginalDigest = lowSEHFrameDependencyDigest(F);
  const auto Copy = F;
  F.Blocks.front().ExceptionalSuccs.front().SharedContexts =
      std::make_shared<const ExceptionalEdgeContextDomain>(
          std::vector<ExceptionalEdgeContext>{{0, 4}, {1, 3}});
  EXPECT_NE(OriginalDigest, lowSEHFrameDependencyDigest(F));
  EXPECT_NE(A, F.Blocks.front().ExceptionalSuccs.front());
  F = {};
  A = {};
  B = {};
  EXPECT_EQ(OriginalDigest, lowSEHFrameDependencyDigest(Copy));
  const auto Contexts = Copy.Blocks.front().ExceptionalSuccs.front().contexts();
  EXPECT_EQ(std::vector(Contexts.begin(), Contexts.end()), Values);
  EXPECT_THROW(ExceptionalEdgeContextDomain({}), std::invalid_argument);
  ExceptionalEdge Single(1, 0x2000, ExceptionalEdgeKind::CxxCleanup, 0, 3);
  auto SharedSingle = Single;
  SharedSingle.SharedContexts =
      std::make_shared<const ExceptionalEdgeContextDomain>(
          std::vector<ExceptionalEdgeContext>{{0, 3}});
  EXPECT_EQ(Single, SharedSingle);
  ASSERT_NE(SharedSingle.singleContext(), nullptr);
  EXPECT_EQ(SharedSingle.singleContext()->State, 3);
}

TEST(RegistrationEdges, PublishedEdgesOutliveIndependentAnalyses) {
  std::vector<std::vector<ExceptionalEdge>> Retained;
  for (unsigned States : {9u, 257u}) {
    auto Image = cleanupImage(States, 4, true);
    auto F = build(Image);
    ASSERT_TRUE(F.RegistrationStates);
    const auto State =
        std::find_if(F.RegistrationStates->Blocks.begin(),
                     F.RegistrationStates->Blocks.end(),
                     [](const auto &S) { return S.CanDispatch && S.Unknown; });
    ASSERT_NE(State, F.RegistrationStates->Blocks.end());
    const auto Block =
        std::find_if(F.Blocks.begin(), F.Blocks.end(),
                     [&](const auto &B) { return B.Id == State->BlockId; });
    ASSERT_NE(Block, F.Blocks.end());
    Retained.push_back(Block->ExceptionalSuccs);
  }
  ASSERT_EQ(Retained[0].size(), 1u);
  ASSERT_EQ(Retained[1].size(), 1u);
  EXPECT_EQ(Retained[0][0].contexts().size(), 9u + 5u);
  EXPECT_EQ(Retained[1][0].contexts().size(), 257u + 129u);
  EXPECT_NE(Retained[0][0].SharedContexts, Retained[1][0].SharedContexts);
  const auto Copied = Retained;
  Retained.clear();
  EXPECT_EQ(flatten(Copied[0]).size(), 14u);
  EXPECT_EQ(flatten(Copied[1]).size(), 386u);
}
} // namespace
