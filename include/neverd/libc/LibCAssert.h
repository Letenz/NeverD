#ifndef NEVERD_LIBC_LIBCASSERT_H
#define NEVERD_LIBC_LIBCASSERT_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C assert.h — runtime assertions
inline constexpr std::string_view kAssertHeader = "assert.h";

inline constexpr std::array kAssertFunctions = {
    "__assert_fail",
    "__assert_rtn",
    "__assert",
    "assert",
};

/// Fixed arity of the assert.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kAssertArity = std::to_array<LibCArityEntry>({
    {"assert_fail", {4, 0}},
    {"assert", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCASSERT_H
