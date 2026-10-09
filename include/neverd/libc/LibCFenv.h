#ifndef NEVERD_LIBC_LIBCFENV_H
#define NEVERD_LIBC_LIBCFENV_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C fenv.h — floating-point environment
inline constexpr std::string_view kFenvHeader = "fenv.h";

inline constexpr std::array kFenvFunctions = {
    "feclearexcept", "fedisableexcept", "feenableexcept",   "fegetenv",
    "fegetexcept",   "fegetexceptflag", "fegetround",       "feholdexcept",
    "feraiseexcept", "fesetenv",        "fesetexcept",      "fesetexceptflag",
    "fesetround",    "fetestexcept",    "fetestexceptflag", "feupdateenv",
};

/// Fixed arity of the fenv.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kFenvArity = std::to_array<LibCArityEntry>({
    {"feclearexcept", {1, 0}},
    {"fedisableexcept", {1, 0}},
    {"feenableexcept", {1, 0}},
    {"fegetenv", {1, 0}},
    {"fegetexcept", {0, 0}},
    {"fegetexceptflag", {2, 0}},
    {"fegetround", {0, 0}},
    {"feholdexcept", {1, 0}},
    {"feraiseexcept", {1, 0}},
    {"fesetenv", {1, 0}},
    {"fesetexcept", {1, 0}},
    {"fesetexceptflag", {2, 0}},
    {"fesetround", {1, 0}},
    {"fetestexcept", {1, 0}},
    {"fetestexceptflag", {2, 0}},
    {"feupdateenv", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFENV_H
