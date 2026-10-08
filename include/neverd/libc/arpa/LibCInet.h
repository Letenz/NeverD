#ifndef NEVERD_LIBC_ARPA_LIBCINET_H
#define NEVERD_LIBC_ARPA_LIBCINET_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX arpa/inet.h — internet address manipulation
inline constexpr std::string_view kArpaInetHeader = "arpa/inet.h";

inline constexpr std::array kArpaInetFunctions = {
    "htonl",     "htons",     "inet_addr", "inet_aton", "inet_ntoa",
    "inet_ntop", "inet_pton", "ntohl",     "ntohs",
};

/// Fixed arity of the arpa/inet.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kArpaInetArity = std::to_array<LibCArityEntry>({
    {"htonl", {1, 0}},
    {"htons", {1, 0}},
    {"inet_addr", {1, 0}},
    {"inet_aton", {2, 0}},
    {"inet_ntoa", {1, 0}},
    {"inet_ntop", {4, 0}},
    {"inet_pton", {3, 0}},
    {"ntohl", {1, 0}},
    {"ntohs", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_ARPA_LIBCINET_H
