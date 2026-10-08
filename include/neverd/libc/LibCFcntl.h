#ifndef NEVERD_LIBC_LIBCFCNTL_H
#define NEVERD_LIBC_LIBCFCNTL_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX fcntl.h — file control
inline constexpr std::string_view kFcntlHeader = "fcntl.h";

inline constexpr std::array kFcntlFunctions = {
    "creat",
    "fcntl",
    "open",
    "openat",
};

/// Fixed arity of the fcntl.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kFcntlArity = std::to_array<LibCArityEntry>({
    {"creat", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFCNTL_H
