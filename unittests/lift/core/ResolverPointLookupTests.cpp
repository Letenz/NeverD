//===- ResolverPointLookupTests.cpp - Immutable point-index lookups -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "jumptable/ResolverPointLookup.h"

#include <algorithm>
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
