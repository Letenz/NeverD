#ifndef NEVERD_LIBC_SYS_LIBCSTATVFS_H
#define NEVERD_LIBC_SYS_LIBCSTATVFS_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/statvfs.h — filesystem statistics
inline constexpr std::string_view kSysStatvfsHeader = "sys/statvfs.h";

inline constexpr std::array kSysStatvfsFunctions = {
    "fstatvfs",
    "statvfs",
};

/// Fixed arity of the sys/statvfs.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysStatvfsArity = std::to_array<LibCArityEntry>({
    {"fstatvfs", {2, 0}},
    {"statvfs", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSTATVFS_H
