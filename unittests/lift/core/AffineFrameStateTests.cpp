//===- AffineFrameStateTests.cpp - Cyclic frame identity proofs
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "jumptable/AffineFrameState.h"

#include <array>
#include <functional>
#include <limits>
#include <random>
#include <set>
#include <utility>

using Frame = neverd::detail::AffineFrameState;

namespace {
struct Proof {
  size_t Work = 0;
  size_t Remaining = std::numeric_limits<size_t>::max();
  bool consume(size_t Amount) {
    Work += Amount;
    if (Amount > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Amount;
    return true;
  }
  const std::function<bool(size_t)> Consume = [this](size_t Amount) {
    return consume(Amount);
  };
  Frame Graph{Consume};
};
} // namespace

TEST(AffineFrameState, BalancedLoopPreservesDifferentIntermediateEpochs) {
  Proof P;
  const auto Header = P.Graph.reference();
  const auto Pushed = P.Graph.adjust(Header, 8, true);
  const auto Popped = P.Graph.adjust(Pushed, 8, false);
  ASSERT_TRUE(
      P.Graph.define(Header, P.Graph.merge({Frame::constant(-32), Popped})));
  const auto H = P.Graph.value(Header);
  ASSERT_EQ(H.K, Frame::Kind::Value);
  EXPECT_EQ(H.Offset, -32);
  EXPECT_EQ(P.Graph.value(Pushed).Offset, -40);
}

TEST(AffineFrameState, NonzeroCycleDeltaInvalidatesItsUsers) {
  Proof P;
  const auto Header = P.Graph.reference();
  const auto Backedge = P.Graph.adjust(Header, 8, true);
  const auto User = P.Graph.adjust(Header, 16, false);
  ASSERT_TRUE(
      P.Graph.define(Header, P.Graph.merge({Frame::constant(0), Backedge})));
  EXPECT_EQ(P.Graph.value(Header).K, Frame::Kind::Invalid);
  EXPECT_EQ(P.Graph.value(User).K, Frame::Kind::Invalid);
}

TEST(AffineFrameState, UnanchoredCycleCannotBorrowAnotherPredecessorsValue) {
  Proof P;
  const auto Cycle = P.Graph.reference();
  ASSERT_TRUE(P.Graph.define(Cycle, Cycle));
  const auto Join = P.Graph.merge({Frame::constant(0), Cycle});
  EXPECT_EQ(P.Graph.value(Cycle).K, Frame::Kind::Invalid);
  EXPECT_EQ(P.Graph.value(Join).K, Frame::Kind::Invalid);
}

TEST(AffineFrameState, EveryAnchorAndIncomingPathMustAgree) {
  for (bool Reverse : {false, true}) {
    Proof P;
    auto Left = P.Graph.reference(), Right = P.Graph.reference();
    if (Reverse)
      std::swap(Left, Right);
    ASSERT_TRUE(
        P.Graph.define(Left, P.Graph.merge({Frame::constant(4), Right})));
    ASSERT_TRUE(
        P.Graph.define(Right, P.Graph.merge({Frame::constant(8), Left})));
    EXPECT_EQ(P.Graph.value(Left).K, Frame::Kind::Invalid);
    EXPECT_EQ(P.Graph.value(Right).K, Frame::Kind::Invalid);
    EXPECT_EQ(
        P.Graph.value(P.Graph.merge({Frame::constant(0), Frame::invalid()})).K,
        Frame::Kind::Invalid);
  }
}

TEST(AffineFrameState, OverflowingIntermediateCannotBeCancelledByLaterOffset) {
  for (bool Subtract : {false, true}) {
    Proof P;
    const auto Root = P.Graph.reference();
    const auto Overflow = P.Graph.adjust(Root, 1, Subtract);
    const auto Cancelled = P.Graph.adjust(Overflow, 1, !Subtract);
    ASSERT_TRUE(P.Graph.define(
        Root, Frame::constant(Subtract ? std::numeric_limits<int64_t>::min()
                                       : std::numeric_limits<int64_t>::max())));
    EXPECT_EQ(P.Graph.value(Overflow).K, Frame::Kind::Invalid);
    EXPECT_EQ(P.Graph.value(Cancelled).K, Frame::Kind::Invalid);
  }
}

TEST(AffineFrameState, SharedCyclicDiamondsHaveLinearEvidenceCost) {
  Proof P;
  const auto Header = P.Graph.reference();
  auto Tail = Header;
  constexpr size_t Diamonds = 1024;
  for (size_t I = 0; I < Diamonds; ++I) {
    const auto Left = P.Graph.adjust(Tail, 8, false);
    const auto Right = P.Graph.adjust(Tail, 16, false);
    Tail = P.Graph.merge(
        {P.Graph.adjust(Left, 8, true), P.Graph.adjust(Right, 16, true)});
  }
  ASSERT_TRUE(
      P.Graph.define(Header, P.Graph.merge({Frame::constant(-64), Tail})));
  const auto Result = P.Graph.value(Tail);
  ASSERT_EQ(Result.K, Frame::Kind::Value);
  EXPECT_EQ(Result.Offset, -64);
  EXPECT_LT(P.Work, 512 * Diamonds);
}

TEST(AffineFrameState, ExhaustionNeverPublishesAPartialCycleProof) {
  const auto Run = [](size_t Budget) {
    Proof P;
    P.Remaining = Budget;
    const auto Header = P.Graph.reference();
    const auto Backedge =
        P.Graph.adjust(P.Graph.adjust(Header, 8, true), 8, false);
    P.Graph.define(Header, P.Graph.merge({Frame::constant(-16), Backedge}));
    const auto Result = P.Graph.value(Header);
    return std::pair{Result.K, P.Work};
  };
  const auto [Kind, Required] = Run(std::numeric_limits<size_t>::max());
  ASSERT_EQ(Kind, Frame::Kind::Value);
  EXPECT_EQ(Run(Required).first, Frame::Kind::Value);
  for (size_t Budget = 0; Budget < Required; ++Budget)
    EXPECT_EQ(Run(Budget).first, Frame::Kind::Invalid) << Budget;
}

TEST(AffineFrameState, CachedGraphCanGrowAndBeRetired) {
  Proof P;
  const auto Root = P.Graph.reference();
  ASSERT_TRUE(P.Graph.define(Root, Frame::constant(-8)));
  ASSERT_EQ(P.Graph.value(Root).Offset, -8);
  const auto Next = P.Graph.adjust(Root, 4, false);
  ASSERT_EQ(P.Graph.value(Next).K, Frame::Kind::Value);
  EXPECT_EQ(P.Graph.value(Next).Offset, -4);
  P.Graph.clear();
  const auto NewRoot = P.Graph.reference();
  ASSERT_TRUE(P.Graph.define(NewRoot, Frame::constant(16)));
  EXPECT_EQ(P.Graph.value(NewRoot).Offset, 16);
}

TEST(AffineFrameState, IncrementalQueriesDoNotResolveOldEquationsAgain) {
  Proof P;
  const auto Root = P.Graph.reference();
  ASSERT_TRUE(P.Graph.define(Root, Frame::constant(-32)));
  ASSERT_EQ(P.Graph.value(Root).K, Frame::Kind::Value);
  constexpr size_t Queries = 1024;
  for (size_t I = 0; I < Queries; ++I) {
    const auto Query = P.Graph.adjust(Root, I, false);
    ASSERT_EQ(P.Graph.value(Query).K, Frame::Kind::Value);
    EXPECT_EQ(P.Graph.value(Query).Offset, static_cast<int64_t>(I) - 32);
  }
  EXPECT_LT(P.Work, Queries * 128);
}

TEST(AffineFrameState, DefiningAPreviouslyUnanchoredNodeRechecksOldUsers) {
  Proof P;
  const auto Root = P.Graph.reference();
  const auto User = P.Graph.adjust(Root, 16, true);
  EXPECT_EQ(P.Graph.value(User).K, Frame::Kind::Invalid);
  ASSERT_TRUE(P.Graph.define(Root, Frame::constant(8)));
  EXPECT_EQ(P.Graph.value(User).K, Frame::Kind::Value);
  EXPECT_EQ(P.Graph.value(User).Offset, -8);
}

TEST(AffineFrameState, CyclicEquationsAgreeWithIndependentPathConstraints) {
  struct Equation {
    std::optional<int64_t> Constant;
    bool Invalid = false;
    std::vector<std::pair<size_t, int64_t>> Inputs;
  };
  std::mt19937 Random(4919);
  constexpr size_t Count = 5;
  for (unsigned Trial = 0; Trial < 512; ++Trial) {
    SCOPED_TRACE(Trial);
    std::array<Equation, Count> Equations;
    Proof P;
    std::array<Frame::Result, Count> Nodes;
    for (auto &N : Nodes)
      N = P.Graph.reference();
    for (size_t I = 0; I < Count; ++I) {
      Equation &E = Equations[I];
      std::vector<Frame::Result> Inputs;
      if (Random() % 3 == 0) {
        E.Constant = static_cast<int64_t>(Random() % 7) - 3;
        Inputs.push_back(Frame::constant(*E.Constant));
      }
      E.Invalid = Random() % 10 == 0;
      if (E.Invalid)
        Inputs.push_back(Frame::invalid());
      const size_t Edges = Random() % 3;
      for (size_t J = 0; J < Edges; ++J) {
        const size_t Source = Random() % Count;
        const int64_t Delta = static_cast<int64_t>(Random() % 3) - 1;
        E.Inputs.emplace_back(Source, Delta);
        Inputs.push_back(P.Graph.adjust(Nodes[Source], Delta, false));
      }
      ASSERT_TRUE(P.Graph.define(Nodes[I], P.Graph.merge(Inputs)));
    }
    const auto Expected = [&](size_t Root) -> std::optional<int64_t> {
      // Walk equations backward, assigning each dependency an offset from
      // the queried state. Conflicting paths or literal anchors refute it.
      std::array<std::optional<int64_t>, Count> Offsets;
      Offsets[Root] = 0;
      std::vector<size_t> Work{Root};
      std::optional<int64_t> Anchor;
      for (size_t Next = 0; Next < Work.size(); ++Next) {
        const size_t I = Work[Next];
        const Equation &E = Equations[I];
        if (E.Invalid)
          return std::nullopt;
        if (E.Constant) {
          const int64_t Value = *E.Constant - *Offsets[I];
          if (Anchor && *Anchor != Value)
            return std::nullopt;
          Anchor = Value;
        }
        for (const auto &[Source, Delta] : E.Inputs) {
          const int64_t Offset = *Offsets[I] - Delta;
          if (Offsets[Source] && *Offsets[Source] != Offset)
            return std::nullopt;
          if (!Offsets[Source]) {
            Offsets[Source] = Offset;
            Work.push_back(Source);
          }
        }
      }
      // Every dependency needs a path to an anchor: another merge arm cannot
      // lend a value to a disconnected, undefined cycle.
      for (size_t Start : Work) {
        std::set<size_t> Seen{Start};
        std::vector<size_t> Search{Start};
        bool Anchored = false;
        for (size_t Next = 0; Next < Search.size(); ++Next) {
          const auto &E = Equations[Search[Next]];
          Anchored |= E.Constant.has_value();
          for (const auto &[Source, Delta] : E.Inputs)
            if (Seen.insert(Source).second)
              Search.push_back(Source);
        }
        if (!Anchored)
          return std::nullopt;
      }
      return Anchor;
    };
    for (size_t I = 0; I < Count; ++I) {
      const auto Want = Expected(I);
      const auto Actual = P.Graph.value(Nodes[I]);
      ASSERT_EQ(Actual.K == Frame::Kind::Value, Want.has_value()) << I;
      if (Want)
        EXPECT_EQ(Actual.Offset, *Want) << I;
    }
  }
}
