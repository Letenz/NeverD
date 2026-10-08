#ifndef NEVERD_LIBC_SYS_LIBCWAIT_H
#define NEVERD_LIBC_SYS_LIBCWAIT_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/wait.h — process wait
inline constexpr std::string_view kSysWaitHeader = "sys/wait.h";

inline constexpr std::array kSysWaitFunctions = {
    "wait",
    "wait4",
    "waitid",
    "waitpid",
};

/// Fixed arity of the sys/wait.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysWaitArity = std::to_array<LibCArityEntry>({
    {"wait", {1, 0}},
    {"wait4", {4, 0}},
    {"waitid", {4, 0}},
    {"waitpid", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCWAIT_H
