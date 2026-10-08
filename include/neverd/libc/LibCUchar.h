#ifndef NEVERD_LIBC_LIBCUCHAR_H
#define NEVERD_LIBC_LIBCUCHAR_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C uchar.h — UTF-16 and UTF-32 conversions
inline constexpr std::string_view kUcharHeader = "uchar.h";

inline constexpr std::array kUcharFunctions = {
    "c16rtomb",
    "c32rtomb",
    "mbrtoc16",
    "mbrtoc32",
};

/// Fixed arity of the uchar.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kUcharArity = std::to_array<LibCArityEntry>({
    {"mbrtoc16", {4, 0}},
    {"c16rtomb", {3, 0}},
    {"mbrtoc32", {4, 0}},
    {"c32rtomb", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCUCHAR_H
