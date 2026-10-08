#ifndef NEVERD_LIBC_SYS_LIBCRANDOM_H
#define NEVERD_LIBC_SYS_LIBCRANDOM_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// Linux sys/random.h — random number generation
inline constexpr std::string_view kSysRandomHeader = "sys/random.h";

inline constexpr std::array kSysRandomFunctions = {
    "getrandom",
};

/// Fixed arity of the sys/random.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysRandomArity = std::to_array<LibCArityEntry>({
    {"getrandom", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCRANDOM_H
