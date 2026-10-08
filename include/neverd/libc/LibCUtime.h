#ifndef NEVERD_LIBC_LIBCUTIME_H
#define NEVERD_LIBC_LIBCUTIME_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX utime.h — file access and modification times
inline constexpr std::string_view kUtimeHeader = "utime.h";

inline constexpr std::array kUtimeFunctions = {
    "utime",
};

/// Fixed arity of the utime.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kUtimeArity = std::to_array<LibCArityEntry>({
    {"utime", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCUTIME_H
