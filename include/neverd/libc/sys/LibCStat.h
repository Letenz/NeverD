#ifndef NEVERD_LIBC_SYS_LIBCSTAT_H
#define NEVERD_LIBC_SYS_LIBCSTAT_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/stat.h — file status
inline constexpr std::string_view kSysStatHeader = "sys/stat.h";

inline constexpr std::array kSysStatFunctions = {
    "chmod", "fchmod",  "fchmodat", "fstat", "fstatat", "futimens",  "lstat",
    "mkdir", "mkdirat", "mkfifo",   "stat",  "umask",   "utimensat",
};

/// Fixed arity of the sys/stat.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysStatArity = std::to_array<LibCArityEntry>({
    {"chmod", {2, 0}},
    {"fchmod", {2, 0}},
    {"fchmodat", {4, 0}},
    {"fstat", {2, 0}},
    {"fstatat", {4, 0}},
    {"lstat", {2, 0}},
    {"mkdirat", {3, 0}},
    {"mkfifo", {2, 0}},
    {"stat", {2, 0}},
    {"umask", {1, 0}},
    {"utimensat", {4, 0}},
    {"futimens", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSTAT_H
