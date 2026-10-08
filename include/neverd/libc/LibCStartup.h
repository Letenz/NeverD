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
inline constexpr auto kStartupPrototypes = std::to_array<LibCPrototype>({
    {"libc_start_main",
     "int",
     {"int (*)(int, char **, char **)", "int", "char **", "void (*)(void)",
      "void (*)(void)", "void (*)(void)", "void *"},
     7},
});

/// Their arities, which their integer and pointer parameters give.
inline constexpr auto kStartupArity = [] {
  std::array<LibCArityEntry, kStartupPrototypes.size()> Arity{};
  for (size_t I = 0; I < kStartupPrototypes.size(); ++I)
    Arity[I] = {kStartupPrototypes[I].Name,
                {kStartupPrototypes[I].ParamCount, 0}};
  return Arity;
}();

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTARTUP_H
