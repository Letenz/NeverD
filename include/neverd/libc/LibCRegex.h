#ifndef NEVERD_LIBC_LIBCREGEX_H
#define NEVERD_LIBC_LIBCREGEX_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX regex.h — regular expressions
inline constexpr std::string_view kRegexHeader = "regex.h";

inline constexpr std::array kRegexFunctions = {
    "re_compile_pattern", "re_search", "re_set_syntax", "regcomp",
    "regerror",           "regexec",   "regfree",
};

/// Fixed arity of the regex.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kRegexArity = std::to_array<LibCArityEntry>({
    {"regcomp", {3, 0}},
    {"regerror", {4, 0}},
    {"regexec", {5, 0}},
    {"regfree", {1, 0}},
    {"re_compile_pattern", {3, 0}},
    {"re_search", {6, 0}},
    {"re_set_syntax", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCREGEX_H
