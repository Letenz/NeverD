//===- CaseListTests.cpp - Switch values as one list ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/CaseList.h"

#include <cstdint>

using namespace neverd;

namespace {

TEST(CaseList, WritesRunsOfThreeOrMoreAsRanges) {
  EXPECT_EQ(formatCaseList({}), "");
  EXPECT_EQ(formatCaseList({7}), "7");
  EXPECT_EQ(formatCaseList({11, 12}), "11,12");
  // Sorted and without repeats, as IDA lists a default case's values.
  EXPECT_EQ(formatCaseList({14, 12, 11, 13, 61, 40, 41, 42, 11}),
            "11-14,40-42,61");
  EXPECT_EQ(formatCaseList({11, 12, 14, 15, 16, 17, 33}), "11,12,14-17,33");
}

TEST(CaseList, StopsARunAtTheLargestValue) {
  EXPECT_EQ(formatCaseList({INT64_MAX - 2, INT64_MAX - 1, INT64_MAX}),
            "9223372036854775805-9223372036854775807");
  EXPECT_EQ(formatCaseList({INT64_MIN, INT64_MAX}),
            "-9223372036854775808,9223372036854775807");
}

} // namespace
