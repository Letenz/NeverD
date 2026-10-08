#ifndef NEVERD_LIBC_SYS_LIBCRESOURCE_H
#define NEVERD_LIBC_SYS_LIBCRESOURCE_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/resource.h — resource limits and usage
inline constexpr std::string_view kSysResourceHeader = "sys/resource.h";

inline constexpr std::array kSysResourceFunctions = {
    "getrlimit",
    "getrusage",
    "setrlimit",
};

/// Fixed arity of the sys/resource.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysResourceArity = std::to_array<LibCArityEntry>({
    {"getrlimit", {2, 0}},
    {"getrusage", {2, 0}},
    {"setrlimit", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCRESOURCE_H
