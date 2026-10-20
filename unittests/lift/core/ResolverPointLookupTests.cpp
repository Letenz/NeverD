//===- ResolverPointLookupTests.cpp - Immutable point-index lookups -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "jumptable/ResolverPointLookup.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <vector>

using namespace neverd;

namespace {

using Point = std::pair<va_t, int>;
using Location = std::pair<int, int>;
using Index = std::map<Point, Location>;

Index makeIndex(unsigned Count) {
  Index Result;
  for (unsigned I = 0; I < Count; ++I)
    Result.emplace(Point{uint64_t{I % 64} * 16 | (uint64_t{I / 64} << 32),
                         static_cast<int>(I % 7)},
                   Location{static_cast<int>(I / 11), static_cast<int>(I)});
  return Result;
}

} // namespace

TEST(ResolverPointLookup, ExactKeysSurviveCollisionsAndMissingQueries) {
  // More points than the bounded table can retain, including identical low
  // address bits. Independent expected coordinates come from the ordered map.
  auto Points = makeIndex(1024);
  Points.emplace(Point{InvalidVA, -1}, Location{17, 23});
  Points.emplace(Point{0, std::numeric_limits<int>::max()}, Location{29, 31});
  std::vector<Point> Order;
  for (const auto &[Key, Value] : Points)
    Order.push_back(Key);
  detail::ResolverPointLookup Lookup;
  for (unsigned Pass = 0; Pass < 3; ++Pass) {
    for (Point Key : Order) {
      const auto Expected = Points.at(Key);
      const auto Saved = Lookup.find(Points, Key);
      ASSERT_EQ(Saved, Expected);
      EXPECT_EQ(Lookup.find(Points, Key), Saved);
      // The instruction sequence is part of the identity even at one address.
      const Point Missing{Key.first, std::numeric_limits<int>::min()};
      EXPECT_FALSE(Lookup.find(Points, Missing));
      EXPECT_FALSE(Lookup.find(Points, Missing));
      EXPECT_EQ(Saved, Expected);
      EXPECT_EQ(Lookup.find(Points, Key), Saved);
    }
    std::reverse(Order.begin(), Order.end());
    std::rotate(Order.begin(), Order.begin() + 137, Order.end());
  }
}

TEST(ResolverPointLookup, DifferentIndexesCannotShareCoordinates) {
  const auto First = makeIndex(128);
  auto Other = First;
  for (auto &[Key, Value] : Other)
    Value.first += 1000;
  detail::ResolverPointLookup Lookup;
  for (const Index *Points :
       {&First, static_cast<const Index *>(&Other), &First})
    for (const auto &[Key, Value] : *Points)
      EXPECT_EQ(Lookup.find(*Points, Key), Value);
}

TEST(ResolverPointLookup, RepeatedQueriesKeepCollidingCoordinatesAndAbsence) {
  struct CountingIndex {
    Index Values = makeIndex(128);
    mutable unsigned Searches = 0;
    size_t size() const { return Values.size(); }
    auto end() const { return Values.end(); }
    auto find(Point Key) const {
      ++Searches;
      return Values.find(Key);
    }
  } Points;
  const std::array<Point, 4> Queries = {
      Point{0x1000, 0}, Point{0x10000001000, 0}, Point{0x20000001000, 0},
      Point{0x30000001000, std::numeric_limits<int>::min()}};
  for (unsigned I = 0; I < 3; ++I)
    Points.Values.emplace(
        Queries[I], Location{static_cast<int>(I + 1), static_cast<int>(I * 7)});

  detail::ResolverPointLookup Lookup;
  for (unsigned I = 0; I < 256; ++I)
    ASSERT_EQ(Lookup.find(Points, Queries.front()),
              Points.Values.at(Queries.front()));
  auto Check = [&](Point Key) {
    const auto Expected = Points.Values.find(Key);
    const auto Actual = Lookup.find(Points, Key);
    if (Expected == Points.Values.end())
      EXPECT_FALSE(Actual);
    else
      EXPECT_EQ(Actual, Expected->second);
  };
  for (Point Key : Queries)
    Check(Key);
  const unsigned Before = Points.Searches;
  // A small repeatedly used working set, including a missing point, should
  // not keep evicting itself merely because its address bits collide.
  for (unsigned Pass = 0; Pass < 128; ++Pass)
    for (Point Key : Queries)
      Check(Key);
  EXPECT_EQ(Points.Searches, Before);
}

TEST(ResolverPointLookup, MovingTheGraphPreservesReturnedCoordinates) {
  auto Points = makeIndex(128);
  const Point Key = Points.rbegin()->first;
  detail::ResolverPointLookup Lookup;
  for (const auto &[Point, Value] : Points)
    EXPECT_EQ(Lookup.find(Points, Point), Value);
  const auto Saved = Lookup.find(Points, Key);
  ASSERT_TRUE(Saved);
  auto MovedPoints = std::move(Points);
  auto MovedLookup = std::move(Lookup);
  EXPECT_EQ(MovedLookup.find(MovedPoints, Key), Saved);
  const Index Empty;
  EXPECT_FALSE(MovedLookup.find(Empty, Key));
  EXPECT_EQ(MovedLookup.find(MovedPoints, Key), Saved);
}
