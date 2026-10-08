#ifndef NEVERD_LIBC_LIBCSTDIOEXT_H
#define NEVERD_LIBC_LIBCSTDIOEXT_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// glibc stdio_ext.h — stream buffer and state queries
inline constexpr std::string_view kStdioExtHeader = "stdio_ext.h";

inline constexpr std::array kStdioExtFunctions = {
    "__fbufsize", "__flbf",        "__fpending",  "__fpurge",   "__freadable",
    "__freading", "__fsetlocking", "__fwritable", "__fwriting",
};

/// Fixed arity of the stdio_ext.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kStdioExtArity = std::to_array<LibCArityEntry>({
    {"fpending", {1, 0}},
    {"freading", {1, 0}},
    {"fwriting", {1, 0}},
    {"freadable", {1, 0}},
    {"fwritable", {1, 0}},
    {"flbf", {1, 0}},
    {"fpurge", {1, 0}},
    {"fbufsize", {1, 0}},
    {"fsetlocking", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTDIOEXT_H
