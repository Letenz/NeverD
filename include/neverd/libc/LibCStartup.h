#ifndef NEVERD_LIBC_LIBCSTARTUP_H
#define NEVERD_LIBC_LIBCSTARTUP_H

#include "neverd/libc/LibCNames.h"

#include <array>

namespace neverd::libc {

/// The C library routines a program's start files and compiler-generated code
/// call and no standard header declares.
///
/// `_start` (crt1.o) passes __libc_start_main main, argc, argv, init, fini,
/// rtld_fini and stack_end, the signature the Linux Standard Base specifies
/// and glibc implements.  musl's routine declares the first six; the seventh
/// argument it does not read changes nothing for it.  uClibc's __uClibc_main,
/// bionic's __libc_init and FreeBSD's __libc_start1 are the same entries of
/// those C libraries.  crtbegin.o registers and finalizes the module through
/// __cxa_atexit, __cxa_finalize, the libitm clone tables and the libgcc frame
/// registry, and calls __gmon_start__ when profiling links it.  The ctype and
/// errno macros of glibc and bionic read through the *_loc and __errno
/// routines, and the stack protector fails through __stack_chk_fail.
inline constexpr auto kStartupPrototypes = std::to_array<LibCPrototype>({
    makeLibCPrototype("__libc_start_main", "int",
                      {"int (*)(int, char **, char **)", "int", "char **",
                       "void (*)(void)", "void (*)(void)", "void (*)(void)",
                       "void *"}),
    makeLibCPrototype("__uClibc_main", "void",
                      {"int (*)(int, char **, char **)", "int", "char **",
                       "void (*)(void)", "void (*)(void)", "void (*)(void)",
                       "void *"}),
    makeLibCPrototype("__libc_init", "void",
                      {"void *", "void (*)(void)",
                       "int (*)(int, char **, char **)", "const void *"}),
    makeLibCPrototype("__libc_start1", "void",
                      {"int", "char **", "char **", "void (*)(void)",
                       "int (*)(int, char **, char **)"}),
    makeLibCPrototype("__cxa_atexit", "int",
                      {"void (*)(void *)", "void *", "void *"}),
    makeLibCPrototype("__cxa_thread_atexit_impl", "int",
                      {"void (*)(void *)", "void *", "void *"}),
    makeLibCPrototype("__cxa_finalize", "void", {"void *"}),
    makeLibCPrototype("__gmon_start__", "void", {}),
    makeLibCPrototype("_ITM_registerTMCloneTable", "void", {"void *", "size_t"},
                      "stddef.h"),
    makeLibCPrototype("_ITM_deregisterTMCloneTable", "void", {"void *"}),
    makeLibCPrototype("__register_frame_info", "void",
                      {"const void *", "void *"}),
    makeLibCPrototype("__deregister_frame_info", "void *", {"const void *"}),
    makeLibCPrototype("_Jv_RegisterClasses", "void", {"void *"}),
    makeLibCPrototype("__stack_chk_fail", "void", {}),
    makeLibCPrototype("__stack_chk_fail_local", "void", {}),
    makeLibCPrototype("__errno_location", "int *", {}),
    makeLibCPrototype("__errno", "int *", {}),
    makeLibCPrototype("__ctype_b_loc", "const unsigned short **", {}),
    makeLibCPrototype("__ctype_tolower_loc", "const int32_t **", {}),
    makeLibCPrototype("__ctype_toupper_loc", "const int32_t **", {}),
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTARTUP_H
