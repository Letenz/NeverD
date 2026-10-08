#ifndef NEVERD_LIBC_LIBCNLTYPES_H
#define NEVERD_LIBC_LIBCNLTYPES_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX nl_types.h — message catalogue
inline constexpr std::string_view kNlTypesHeader = "nl_types.h";

inline constexpr std::array kNlTypesFunctions = {
    "catclose",
    "catgets",
    "catopen",
};

/// Fixed arity of the nl_types.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kNlTypesArity = std::to_array<LibCArityEntry>({
    {"catclose", {1, 0}},
    {"catgets", {4, 0}},
    {"catopen", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCNLTYPES_H
