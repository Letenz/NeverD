#ifndef NEVERD_LIBC_LIBCWCHAR_H
#define NEVERD_LIBC_LIBCWCHAR_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C wchar.h — wide-character string and I/O
inline constexpr std::string_view kWcharHeader = "wchar.h";

inline constexpr std::array kWcharFunctions = {
    "btowc",     "mbrlen",   "mbrtowc",    "mbsinit",   "mbsnrtowcs",
    "mbsrtowcs", "wcrtomb",  "wcsnrtombs", "wcsrtombs", "wctob",
    "wcscat",    "wcschr",   "wcscmp",     "wcscoll",   "wcscpy",
    "wcscspn",   "wcsdup",   "wcslcat",    "wcslcpy",   "wcslen",
    "wcsncat",   "wcsncmp",  "wcsncpy",    "wcsnlen",   "wcspbrk",
    "wcsrchr",   "wcsspn",   "wcsstr",     "wcstok",    "wcsxfrm",
    "wcpcpy",    "wcpncpy",  "wmemchr",    "wmemcmp",   "wmemcpy",
    "wmemmove",  "wmempcpy", "wmemset",    "wcstod",    "wcstof",
    "wcstol",    "wcstold",  "wcstoll",    "wcstoul",   "wcstoull",
    "fgetwc",    "fgetws",   "fputwc",     "fputws",    "fwide",
    "getwc",     "getwchar", "putwc",      "putwchar",  "ungetwc",
    "wcwidth",   "wcswidth",
};

/// Fixed arity of the wchar.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kWcharArity = std::to_array<LibCArityEntry>({
    {"btowc", {1, 0}},   {"mbrlen", {3, 0}},     {"mbrtowc", {4, 0}},
    {"mbsinit", {1, 0}}, {"mbsnrtowcs", {5, 0}}, {"mbsrtowcs", {4, 0}},
    {"wcrtomb", {3, 0}}, {"wcsnrtombs", {5, 0}}, {"wcsrtombs", {4, 0}},
    {"wctob", {1, 0}},   {"wcscat", {2, 0}},     {"wcschr", {2, 0}},
    {"wcscmp", {2, 0}},  {"wcscoll", {2, 0}},    {"wcscpy", {2, 0}},
    {"wcscspn", {2, 0}}, {"wcsdup", {1, 0}},     {"wcslcat", {3, 0}},
    {"wcslcpy", {3, 0}}, {"wcslen", {1, 0}},     {"wcsncat", {3, 0}},
    {"wcsncmp", {3, 0}}, {"wcsncpy", {3, 0}},    {"wcsnlen", {2, 0}},
    {"wcspbrk", {2, 0}}, {"wcsrchr", {2, 0}},    {"wcsspn", {2, 0}},
    {"wcsstr", {2, 0}},  {"wcsxfrm", {3, 0}},    {"wcpcpy", {2, 0}},
    {"wcpncpy", {3, 0}}, {"wmemchr", {3, 0}},    {"wmemcmp", {3, 0}},
    {"wmemcpy", {3, 0}}, {"wmemmove", {3, 0}},   {"wmempcpy", {3, 0}},
    {"wmemset", {3, 0}}, {"wcstol", {3, 0}},     {"wcstoll", {3, 0}},
    {"wcstoul", {3, 0}}, {"wcstoull", {3, 0}},   {"fgetwc", {1, 0}},
    {"fgetws", {3, 0}},  {"fputwc", {2, 0}},     {"fputws", {2, 0}},
    {"fwide", {2, 0}},   {"getwc", {1, 0}},      {"getwchar", {0, 0}},
    {"putwc", {2, 0}},   {"putwchar", {1, 0}},   {"ungetwc", {2, 0}},
    {"wcwidth", {1, 0}}, {"wcswidth", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCWCHAR_H
