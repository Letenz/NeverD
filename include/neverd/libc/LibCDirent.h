#ifndef NEVERD_LIBC_LIBCDIRENT_H
#define NEVERD_LIBC_LIBCDIRENT_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX dirent.h — directory operations
inline constexpr std::string_view kDirentHeader = "dirent.h";

inline constexpr std::array kDirentFunctions = {
    "alphasort", "closedir",  "dirfd",   "fdopendir", "opendir", "readdir",
    "readdir_r", "rewinddir", "scandir", "seekdir",   "telldir",
};

/// Fixed arity of the dirent.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kDirentArity = std::to_array<LibCArityEntry>({
    {"alphasort", {2, 0}},
    {"closedir", {1, 0}},
    {"dirfd", {1, 0}},
    {"fdopendir", {1, 0}},
    {"opendir", {1, 0}},
    {"readdir", {1, 0}},
    {"readdir_r", {3, 0}},
    {"scandir", {4, 0}},
    {"seekdir", {2, 0}},
    {"telldir", {1, 0}},
    {"rewinddir", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCDIRENT_H
