#ifndef NEVERD_LIBC_LIBCLOCALE_H
#define NEVERD_LIBC_LIBCLOCALE_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C locale.h — localization
inline constexpr std::string_view kLocaleHeader = "locale.h";

inline constexpr std::array kLocaleFunctions = {
    "duplocale", "freelocale", "localeconv",
    "newlocale", "setlocale",  "uselocale",
};

/// Fixed arity of the locale.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kLocaleArity = std::to_array<LibCArityEntry>({
    {"duplocale", {1, 0}},
    {"freelocale", {1, 0}},
    {"localeconv", {0, 0}},
    {"newlocale", {3, 0}},
    {"setlocale", {2, 0}},
    {"uselocale", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCLOCALE_H
