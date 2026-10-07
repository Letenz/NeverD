//===- NeverDCLIFunctions.h - Functions holding addresses ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Names an address by the function holding it, as the GUI's lists do: a
/// function holds the bytes from its entry to the first of its recorded
/// end, the next function's entry and the end of its segment.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERDCLIFUNCTIONS_H
#define NEVERD_TOOLS_NEVERDCLIFUNCTIONS_H

#include "neverd/sdk/NeverDCAPI.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::cli {

class FunctionLocator {
public:
  explicit FunctionLocator(neverd_session_t Sess);
  /// "name" at a function's entry, "name+0x1A" inside it, or empty.
  std::string locate(neverd_va_t Address) const;

private:
  struct Function {
    neverd_va_t Entry = 0, End = 0;
    std::string Name;
  };
  std::vector<Function> Functions;
};

} // namespace neverd::cli

#endif // NEVERD_TOOLS_NEVERDCLIFUNCTIONS_H
