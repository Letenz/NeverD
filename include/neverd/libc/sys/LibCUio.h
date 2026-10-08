#ifndef NEVERD_LIBC_SYS_LIBCUIO_H
#define NEVERD_LIBC_SYS_LIBCUIO_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/uio.h — scatter/gather I/O
inline constexpr std::string_view kSysUioHeader = "sys/uio.h";

inline constexpr std::array kSysUioFunctions = {
    "readv",
    "writev",
};

/// Fixed arity of the sys/uio.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysUioArity = std::to_array<LibCArityEntry>({
    {"readv", {3, 0}},
    {"writev", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCUIO_H
