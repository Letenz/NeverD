//===- ProducerCorpus.h - Required producer qualification --------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Release qualification fails for missing inputs or unavailable consumers.
/// Ordinary local tests keep their explicit optional-corpus skips.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "gtest/gtest.h"

#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace neverd::web::test {
inline ::testing::AssertionResult corpusPolicy(const char *Mode, bool Available,
                                               const char *Requirement) {
  const std::string_view Policy = Mode ? Mode : "0";
  if (Policy != "0" && Policy != "1")
    return ::testing::AssertionFailure()
           << "NEVERD_REQUIRE_WEB_PRODUCER_CORPUS must be 0 or 1";
  if (Policy == "1" && !Available)
    return ::testing::AssertionFailure()
           << "Required producer qualification is unavailable: " << Requirement;
  return ::testing::AssertionSuccess();
}

inline ::testing::AssertionResult producerPrerequisite(bool Available,
                                                       const char *Name) {
  return corpusPolicy(std::getenv("NEVERD_REQUIRE_WEB_PRODUCER_CORPUS"),
                      Available, Name);
}

inline ::testing::AssertionResult producerCorpus(const char *Name,
                                                 const char *Directory) {
  auto Policy = producerPrerequisite(Directory && *Directory, Name);
  if (!Policy || !Directory || !*Directory)
    return Policy;
  std::error_code EC;
  if (!std::filesystem::is_directory(Directory, EC) || EC)
    return ::testing::AssertionFailure()
           << Name << " must select a readable pinned corpus directory";
  return ::testing::AssertionSuccess();
}
} // namespace neverd::web::test
