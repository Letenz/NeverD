#ifndef NEVERD_LIBC_SYS_LIBCIPC_H
#define NEVERD_LIBC_SYS_LIBCIPC_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/ipc.h — IPC key generation
inline constexpr std::string_view kSysIpcHeader = "sys/ipc.h";

inline constexpr std::array kSysIpcFunctions = {
    "ftok",
};

/// Fixed arity of the sys/ipc.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysIpcArity = std::to_array<LibCArityEntry>({
    {"ftok", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCIPC_H
