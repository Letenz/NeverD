#ifndef NEVERD_LIBC_LIBCINTTYPES_H
#define NEVERD_LIBC_LIBCINTTYPES_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C inttypes.h — integer-type conversions
inline constexpr std::string_view kInttypesHeader = "inttypes.h";

inline constexpr std::array kInttypesFunctions = {
    "imaxabs",
    "imaxdiv",
    "strtoimax",
    "strtoumax",
};

/// Fixed arity of the inttypes.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kInttypesArity = std::to_array<LibCArityEntry>({
    {"strtoimax", {3, 0}},
    {"strtoumax", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCINTTYPES_H
