#ifndef NEVERD_LIBC_LIBCLANGINFO_H
#define NEVERD_LIBC_LIBCLANGINFO_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX langinfo.h — locale information
inline constexpr std::string_view kLanginfoHeader = "langinfo.h";

inline constexpr std::array kLanginfoFunctions = {
    "nl_langinfo",
    "nl_langinfo_l",
};

/// Fixed arity of the langinfo.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kLanginfoArity = std::to_array<LibCArityEntry>({
    {"nl_langinfo", {1, 0}},
    {"nl_langinfo_l", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCLANGINFO_H
