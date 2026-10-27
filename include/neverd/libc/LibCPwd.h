#ifndef NEVERD_LIBC_LIBCPWD_H
#define NEVERD_LIBC_LIBCPWD_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX pwd.h — the password database, with the GNU C Library's own
/// routines of it (fgetpwent, putpwent, getpw).
inline constexpr std::string_view kPwdHeader = "pwd.h";

inline constexpr std::array kPwdFunctions = {
    "endpwent", "fgetpwent",  "fgetpwent_r", "getpw",
    "getpwent", "getpwent_r", "getpwnam",    "getpwnam_r",
    "getpwuid", "getpwuid_r", "putpwent",    "setpwent",
};

/// Fixed arity of the pwd.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kPwdArity = std::to_array<LibCArityEntry>({
    {"endpwent", {0, 0}},
    {"fgetpwent", {1, 0}},
    {"fgetpwent_r", {5, 0}},
    {"getpw", {2, 0}},
    {"getpwent", {0, 0}},
    {"getpwent_r", {4, 0}},
    {"getpwnam", {1, 0}},
    {"getpwnam_r", {5, 0}},
    {"getpwuid", {1, 0}},
    {"getpwuid_r", {5, 0}},
    {"putpwent", {2, 0}},
    {"setpwent", {0, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCPWD_H
