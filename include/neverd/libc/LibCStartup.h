#ifndef NEVERD_LIBC_LIBCSTARTUP_H
#define NEVERD_LIBC_LIBCSTARTUP_H

#include "neverd/libc/LibCNames.h"

#include <array>

namespace neverd::libc {

/// The C library routines a program's start files enter it through.
///
/// `_start` (crt1.o) passes __libc_start_main main, argc, argv, init, fini,
/// rtld_fini and stack_end, the signature the Linux Standard Base specifies
/// and glibc implements.  musl's routine declares the first six; the seventh
/// argument it does not read changes nothing for it.
inline constexpr auto kStartupArity = std::to_array<LibCArityEntry>({
    {"libc_start_main", {7, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTARTUP_H
