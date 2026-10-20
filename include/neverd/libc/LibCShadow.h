#ifndef NEVERD_LIBC_LIBCSHADOW_H
#define NEVERD_LIBC_LIBCSHADOW_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// shadow.h — the shadow password database of the GNU C Library and musl.
inline constexpr std::string_view kShadowHeader = "shadow.h";

inline constexpr std::array kShadowFunctions = {
    "endspent",  "fgetspent",   "fgetspent_r", "getspent", "getspent_r",
    "getspnam",  "getspnam_r",  "lckpwdf",     "putspent", "setspent",
    "sgetspent", "sgetspent_r", "ulckpwdf",
};

/// Fixed arity of the shadow.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kShadowArity = std::to_array<LibCArityEntry>({
    {"endspent", {0, 0}},
    {"fgetspent", {1, 0}},
    {"fgetspent_r", {5, 0}},
    {"getspent", {0, 0}},
    {"getspent_r", {4, 0}},
    {"getspnam", {1, 0}},
    {"getspnam_r", {5, 0}},
    {"lckpwdf", {0, 0}},
    {"putspent", {2, 0}},
    {"setspent", {0, 0}},
    {"sgetspent", {1, 0}},
    {"sgetspent_r", {5, 0}},
    {"ulckpwdf", {0, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSHADOW_H
