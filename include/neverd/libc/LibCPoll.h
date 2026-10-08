#ifndef NEVERD_LIBC_LIBCPOLL_H
#define NEVERD_LIBC_LIBCPOLL_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX poll.h — I/O multiplexing
inline constexpr std::string_view kPollHeader = "poll.h";

inline constexpr std::array kPollFunctions = {
    "poll",
    "ppoll",
};

/// Fixed arity of the poll.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kPollArity = std::to_array<LibCArityEntry>({
    {"poll", {3, 0}},
    {"ppoll", {4, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCPOLL_H
