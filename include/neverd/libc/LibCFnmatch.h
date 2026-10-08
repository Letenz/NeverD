#ifndef NEVERD_LIBC_LIBCFNMATCH_H
#define NEVERD_LIBC_LIBCFNMATCH_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX fnmatch.h — filename pattern matching
inline constexpr std::string_view kFnmatchHeader = "fnmatch.h";

inline constexpr std::array kFnmatchFunctions = {
    "fnmatch",
};

/// Fixed arity of the fnmatch.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kFnmatchArity = std::to_array<LibCArityEntry>({
    {"fnmatch", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFNMATCH_H
