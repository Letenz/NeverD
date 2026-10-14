//===- ProducerCorpusPolicyTests.cpp - Fail-closed qualification --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Required input and consumer omissions are failures, not successful skips.
///
//===----------------------------------------------------------------------===//

#include "ProducerCorpus.h"

namespace {
using namespace neverd::web::test;

TEST(WebProducerCorpusPolicy, RequiredInputsAndConsumersCannotSkip) {
  EXPECT_FALSE(corpusPolicy("1", false, "pinned corpus"));
  EXPECT_FALSE(corpusPolicy("1", false, "native decoder"));
  EXPECT_TRUE(corpusPolicy("1", true, "pinned corpus"));
  EXPECT_TRUE(corpusPolicy("1", true, "native decoder"));
}

TEST(WebProducerCorpusPolicy, LocalOmissionsStayOptionalAndInvalidPolicyFails) {
  EXPECT_TRUE(corpusPolicy(nullptr, false, "pinned corpus"));
  EXPECT_TRUE(corpusPolicy("0", false, "pinned corpus"));
  for (const auto *Mode : {"", "yes", "true", "2"}) {
    EXPECT_FALSE(corpusPolicy(Mode, false, "pinned corpus"));
    EXPECT_FALSE(corpusPolicy(Mode, true, "pinned corpus"));
  }
}
} // namespace
