#ifndef NEVERD_LIBC_SYS_LIBCSEM_H
#define NEVERD_LIBC_SYS_LIBCSEM_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/sem.h — System V semaphores
inline constexpr std::string_view kSysSemHeader = "sys/sem.h";

inline constexpr std::array kSysSemFunctions = {
    "semctl",
    "semget",
    "semop",
};

/// Fixed arity of the sys/sem.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysSemArity = std::to_array<LibCArityEntry>({
    {"semget", {3, 0}},
    {"semop", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSEM_H
