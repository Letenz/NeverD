#ifndef NEVERD_LIBC_SYS_LIBCTIME_H
#define NEVERD_LIBC_SYS_LIBCTIME_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/time.h — time operations
inline constexpr std::string_view kSysTimeHeader = "sys/time.h";

inline constexpr std::array kSysTimeFunctions = {
    "futimesat", "getitimer", "gettimeofday", "setitimer", "utimes",
};

/// Fixed arity of the sys/time.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysTimeArity = std::to_array<LibCArityEntry>({
    {"getitimer", {2, 0}},
    {"gettimeofday", {2, 0}},
    {"setitimer", {3, 0}},
    {"utimes", {2, 0}},
    {"futimesat", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCTIME_H
