#ifndef NEVERD_LIBC_LIBCSTARTUP_H
#define NEVERD_LIBC_LIBCSTARTUP_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <optional>
#include <string_view>

namespace neverd::libc {

/// The function the start routines below call as a program's main: the C
/// standard's hosted main (C11 5.1.2.2.1), with the environment that POSIX
/// systems pass after argv.  kProgramMainDeclaration names its parameters
/// as the standard does.
inline constexpr std::string_view kProgramMainPointer =
    "int (*)(int, char **, char **)";
inline constexpr std::string_view kProgramMainDeclaration =
    "int main(int argc, char **argv, char **envp)";

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
                      {kProgramMainPointer, "int", "char **", "void (*)(void)",
                       "void (*)(void)", "void (*)(void)", "void *"}),
    makeLibCPrototype("__uClibc_main", "void",
                      {kProgramMainPointer, "int", "char **", "void (*)(void)",
                       "void (*)(void)", "void (*)(void)", "void *"}),
    makeLibCPrototype(
        "__libc_init", "void",
        {"void *", "void (*)(void)", kProgramMainPointer, "const void *"}),
    makeLibCPrototype(
        "__libc_start1", "void",
        {"int", "char **", "char **", "void (*)(void)", kProgramMainPointer}),
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

/// The parameter of the start routine \p Name that takes the program's main,
/// or none for another routine.
inline std::optional<unsigned>
startRoutineMainParameter(std::string_view Name) {
  for (const LibCPrototype &Prototype : kStartupPrototypes)
    if (Prototype.Name == Name)
      for (unsigned Index = 0; Index < Prototype.ParamCount; ++Index)
        if (Prototype.Params[Index] == kProgramMainPointer)
          return Index;
  return std::nullopt;
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTARTUP_H
