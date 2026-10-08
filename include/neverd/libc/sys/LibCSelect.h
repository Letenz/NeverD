#ifndef NEVERD_LIBC_SYS_LIBCSELECT_H
#define NEVERD_LIBC_SYS_LIBCSELECT_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/select.h — synchronous I/O multiplexing
inline constexpr std::string_view kSysSelectHeader = "sys/select.h";

inline constexpr std::array kSysSelectFunctions = {
    "pselect",
    "select",
};

/// Fixed arity of the sys/select.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysSelectArity = std::to_array<LibCArityEntry>({
    {"pselect", {6, 0}},
    {"select", {5, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSELECT_H
