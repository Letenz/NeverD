//===- CaseList.h - Switch values as one list -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The values that reach one switch target (case values or table indexes)
/// written as one list, the way the listing comments and the CLI print them:
/// ascending, a run of three or more consecutive values as "first-last" and
/// the rest separated by commas ("11,12,14-33").  Header-only, so the worker
/// shares it without the engine.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_CASELIST_H
#define NEVERD_SUPPORT_CASELIST_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

/// The shortest consecutive run written as a range.
inline constexpr std::size_t CaseRangeMinimum = 3;

/// \p Values ascending and without repeats, runs of CaseRangeMinimum or more
/// consecutive values as ranges.
inline std::string formatCaseList(std::vector<std::int64_t> Values) {
  std::sort(Values.begin(), Values.end());
  Values.erase(std::unique(Values.begin(), Values.end()), Values.end());
  std::string Text;
  for (std::size_t I = 0; I < Values.size();) {
    std::size_t End = I + 1;
    while (End < Values.size() && Values[End - 1] != INT64_MAX &&
           Values[End] == Values[End - 1] + 1)
      ++End;
    if (!Text.empty())
      Text += ',';
    if (End - I >= CaseRangeMinimum) {
      Text += std::to_string(Values[I]) + '-' + std::to_string(Values[End - 1]);
      I = End;
    } else {
      Text += std::to_string(Values[I++]);
    }
  }
  return Text;
}

} // namespace neverd

#endif // NEVERD_SUPPORT_CASELIST_H
