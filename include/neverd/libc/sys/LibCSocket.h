#ifndef NEVERD_LIBC_SYS_LIBCSOCKET_H
#define NEVERD_LIBC_SYS_LIBCSOCKET_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/socket.h — sockets
inline constexpr std::string_view kSysSocketHeader = "sys/socket.h";

inline constexpr std::array kSysSocketFunctions = {
    "accept",      "accept4",    "bind",     "connect",    "getpeername",
    "getsockname", "getsockopt", "listen",   "recv",       "recvfrom",
    "recvmsg",     "send",       "sendmmsg", "sendmsg",    "sendto",
    "setsockopt",  "shutdown",   "socket",   "socketpair",
};

/// Fixed arity of the sys/socket.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysSocketArity = std::to_array<LibCArityEntry>({
    {"accept", {3, 0}},     {"accept4", {4, 0}},     {"bind", {3, 0}},
    {"connect", {3, 0}},    {"getpeername", {3, 0}}, {"getsockname", {3, 0}},
    {"getsockopt", {5, 0}}, {"listen", {2, 0}},      {"recv", {4, 0}},
    {"recvfrom", {6, 0}},   {"recvmsg", {3, 0}},     {"send", {4, 0}},
    {"sendmmsg", {4, 0}},   {"sendmsg", {3, 0}},     {"sendto", {6, 0}},
    {"setsockopt", {5, 0}}, {"shutdown", {2, 0}},    {"socket", {3, 0}},
    {"socketpair", {4, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSOCKET_H
