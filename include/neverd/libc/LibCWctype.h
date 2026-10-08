#ifndef NEVERD_LIBC_LIBCWCTYPE_H
#define NEVERD_LIBC_LIBCWCTYPE_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C wctype.h — wide-character classification
inline constexpr std::string_view kWctypeHeader = "wctype.h";

inline constexpr std::array kWctypeFunctions = {
    "iswalnum",  "iswalpha", "iswblank", "iswcntrl", "iswctype", "iswdigit",
    "iswgraph",  "iswlower", "iswprint", "iswpunct", "iswspace", "iswupper",
    "iswxdigit", "towlower", "towupper", "wctype",
};

/// Fixed arity of the wctype.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kWctypeArity = std::to_array<LibCArityEntry>({
    {"iswalnum", {1, 0}},
    {"iswalpha", {1, 0}},
    {"iswblank", {1, 0}},
    {"iswcntrl", {1, 0}},
    {"iswctype", {2, 0}},
    {"iswdigit", {1, 0}},
    {"iswgraph", {1, 0}},
    {"iswlower", {1, 0}},
    {"iswprint", {1, 0}},
    {"iswpunct", {1, 0}},
    {"iswspace", {1, 0}},
    {"iswupper", {1, 0}},
    {"iswxdigit", {1, 0}},
    {"towlower", {1, 0}},
    {"towupper", {1, 0}},
    {"wctype", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCWCTYPE_H
