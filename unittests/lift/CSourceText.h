//===- CSourceText.h - Finding a function in written C --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Where a function's definition begins in C a writer printed.  The writers
/// declare every function before the first body, so the first occurrence of
/// a name is usually its prototype, not its definition.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_CSOURCETEXT_H
#define NEVERD_UNITTESTS_LIFT_CSOURCETEXT_H

#include <cctype>
#include <string>
#include <string_view>

namespace neverd::test {

/// Where the definition of \p Name begins in \p Source: the first `Name(` at
/// or after \p From whose line opens a body; npos without one.
inline size_t cDefinitionAt(std::string_view Source, std::string_view Name,
                            size_t From = 0) {
  const std::string Needle = std::string(Name) + "(";
  for (size_t At = Source.find(Needle, From); At != std::string_view::npos;
       At = Source.find(Needle, At + 1)) {
    // A longer identifier that ends in the name is another function.
    if (At > 0 && (std::isalnum(static_cast<unsigned char>(Source[At - 1])) ||
                   Source[At - 1] == '_'))
      continue;
    if (Source.find('{', At) < Source.find('\n', At))
      return At;
  }
  return std::string_view::npos;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_LIFT_CSOURCETEXT_H
