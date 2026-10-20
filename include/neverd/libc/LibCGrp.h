#ifndef NEVERD_LIBC_LIBCGRP_H
#define NEVERD_LIBC_LIBCGRP_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX grp.h — the group database, with the GNU C Library's own routines
/// of it (fgetgrent, putgrent).  initgroups, setgroups and getgrouplist are
/// not here: Darwin's <unistd.h> declares them, where glibc's <grp.h> does.
inline constexpr std::string_view kGrpHeader = "grp.h";

inline constexpr std::array kGrpFunctions = {
    "endgrent",   "fgetgrent", "fgetgrent_r", "getgrent",
    "getgrent_r", "getgrgid",  "getgrgid_r",  "getgrnam",
    "getgrnam_r", "putgrent",  "setgrent",
};

/// Fixed arity of the grp.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kGrpArity = std::to_array<LibCArityEntry>({
    {"endgrent", {0, 0}},
    {"fgetgrent", {1, 0}},
    {"fgetgrent_r", {5, 0}},
    {"getgrent", {0, 0}},
    {"getgrent_r", {4, 0}},
    {"getgrgid", {1, 0}},
    {"getgrgid_r", {5, 0}},
    {"getgrnam", {1, 0}},
    {"getgrnam_r", {5, 0}},
    {"putgrent", {2, 0}},
    {"setgrent", {0, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCGRP_H
