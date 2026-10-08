#ifndef NEVERD_LIBC_SYS_LIBCEPOLL_H
#define NEVERD_LIBC_SYS_LIBCEPOLL_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// Linux sys/epoll.h — I/O event notification
inline constexpr std::string_view kSysEpollHeader = "sys/epoll.h";

inline constexpr std::array kSysEpollFunctions = {
    "epoll_create", "epoll_create1", "epoll_ctl",
    "epoll_pwait",  "epoll_pwait2",  "epoll_wait",
};

/// Fixed arity of the sys/epoll.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysEpollArity = std::to_array<LibCArityEntry>({
    {"epoll_create", {1, 0}},
    {"epoll_create1", {1, 0}},
    {"epoll_ctl", {4, 0}},
    {"epoll_pwait", {5, 0}},
    {"epoll_pwait2", {5, 0}},
    {"epoll_wait", {4, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCEPOLL_H
