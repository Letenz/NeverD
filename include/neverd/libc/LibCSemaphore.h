#ifndef NEVERD_LIBC_LIBCSEMAPHORE_H
#define NEVERD_LIBC_LIBCSEMAPHORE_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX semaphore.h — unnamed and named semaphores.  sem_open is variadic
/// (LibCIrregularVarArg.inc).
inline constexpr std::string_view kSemaphoreHeader = "semaphore.h";

inline constexpr std::array kSemaphoreFunctions = {
    "sem_close", "sem_destroy",   "sem_getvalue", "sem_init",   "sem_open",
    "sem_post",  "sem_timedwait", "sem_trywait",  "sem_unlink", "sem_wait",
};

/// Fixed arity of the semaphore.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSemaphoreArity = std::to_array<LibCArityEntry>({
    {"sem_close", {1, 0}},
    {"sem_destroy", {1, 0}},
    {"sem_getvalue", {2, 0}},
    {"sem_init", {3, 0}},
    {"sem_post", {1, 0}},
    {"sem_timedwait", {2, 0}},
    {"sem_trywait", {1, 0}},
    {"sem_unlink", {1, 0}},
    {"sem_wait", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSEMAPHORE_H
