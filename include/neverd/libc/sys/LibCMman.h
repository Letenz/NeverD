#ifndef NEVERD_LIBC_SYS_LIBCMMAN_H
#define NEVERD_LIBC_SYS_LIBCMMAN_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/mman.h — memory management
inline constexpr std::string_view kSysMmanHeader = "sys/mman.h";

inline constexpr std::array kSysMmanFunctions = {
    "madvise", "memfd_create",  "mincore",  "mlock",
    "mlock2",  "mlockall",      "mmap",     "mprotect",
    "mremap",  "msync",         "munlock",  "munlockall",
    "munmap",  "posix_madvise", "shm_open", "shm_unlink",
};

/// Fixed arity of the sys/mman.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysMmanArity = std::to_array<LibCArityEntry>({
    {"madvise", {3, 0}},
    {"memfd_create", {2, 0}},
    {"mincore", {3, 0}},
    {"mlock", {2, 0}},
    {"mlock2", {3, 0}},
    {"mlockall", {1, 0}},
    {"mmap", {6, 0}},
    {"mprotect", {3, 0}},
    {"msync", {3, 0}},
    {"munlock", {2, 0}},
    {"munlockall", {0, 0}},
    {"munmap", {2, 0}},
    {"posix_madvise", {3, 0}},
    {"shm_open", {3, 0}},
    {"shm_unlink", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCMMAN_H
