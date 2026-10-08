#ifndef NEVERD_LIBC_LIBCGETOPT_H
#define NEVERD_LIBC_LIBCGETOPT_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// GNU getopt.h — long command-line options
inline constexpr std::string_view kGetoptHeader = "getopt.h";

inline constexpr std::array kGetoptFunctions = {
    "getopt_long",
    "getopt_long_only",
};

/// Fixed arity of the getopt.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kGetoptArity = std::to_array<LibCArityEntry>({
    {"getopt_long", {5, 0}},
    {"getopt_long_only", {5, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCGETOPT_H
