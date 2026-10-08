#ifndef NEVERD_LIBC_LIBCFORTIFY_H
#define NEVERD_LIBC_LIBCFORTIFY_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// The prefixes of glibc's ISO C conforming aliases: its headers redirect
/// scanf to __isoc99_scanf and, for C23, strtol to __isoc23_strtol.  An alias
/// takes the standard routine's arguments.
inline constexpr std::array<std::string_view, 2> kIsoAliasPrefixes = {
    "isoc99_", "isoc23_"};

/// The C declarations of glibc's _FORTIFY_SOURCE checking routines and of its
/// ISO C conforming aliases, as glibc declares and defines them.  No standard
/// header declares either: the checking routines appear only in the fortified
/// inline wrappers.  A `va_list` passes as the pointer the call holds, a
/// `void *`: C cannot convert a value to the array or record type it is on
/// some targets.  The non-variadic ones' arities are derived from them.
inline constexpr auto kFortifyPrototypes = std::to_array<LibCPrototype>({
    makeLibCPrototype("__printf_chk", "int", {"int", "const char *", "..."}),
    makeLibCPrototype("__fprintf_chk", "int",
                      {"FILE *", "int", "const char *", "..."}, "stdio.h"),
    makeLibCPrototype("__sprintf_chk", "int",
                      {"char *", "int", "size_t", "const char *", "..."},
                      "stddef.h"),
    makeLibCPrototype(
        "__snprintf_chk", "int",
        {"char *", "size_t", "int", "size_t", "const char *", "..."},
        "stddef.h"),
    makeLibCPrototype("__dprintf_chk", "int",
                      {"int", "int", "const char *", "..."}),
    makeLibCPrototype("__asprintf_chk", "int",
                      {"char **", "int", "const char *", "..."}),
    makeLibCPrototype("__syslog_chk", "void",
                      {"int", "int", "const char *", "..."}),
    makeLibCPrototype("__vprintf_chk", "int",
                      {"int", "const char *", "void *"}),
    makeLibCPrototype("__vfprintf_chk", "int",
                      {"FILE *", "int", "const char *", "void *"}, "stdio.h"),
    makeLibCPrototype("__vsprintf_chk", "int",
                      {"char *", "int", "size_t", "const char *", "void *"},
                      "stddef.h"),
    makeLibCPrototype(
        "__vsnprintf_chk", "int",
        {"char *", "size_t", "int", "size_t", "const char *", "void *"},
        "stddef.h"),
    makeLibCPrototype("__vdprintf_chk", "int",
                      {"int", "int", "const char *", "void *"}),
    makeLibCPrototype("__vasprintf_chk", "int",
                      {"char **", "int", "const char *", "void *"}),
    makeLibCPrototype("__vsyslog_chk", "void",
                      {"int", "int", "const char *", "void *"}),
    makeLibCPrototype("__memcpy_chk", "void *",
                      {"void *", "const void *", "size_t", "size_t"},
                      "stddef.h"),
    makeLibCPrototype("__memmove_chk", "void *",
                      {"void *", "const void *", "size_t", "size_t"},
                      "stddef.h"),
    makeLibCPrototype("__mempcpy_chk", "void *",
                      {"void *", "const void *", "size_t", "size_t"},
                      "stddef.h"),
    makeLibCPrototype("__memset_chk", "void *",
                      {"void *", "int", "size_t", "size_t"}, "stddef.h"),
    makeLibCPrototype("__explicit_bzero_chk", "void",
                      {"void *", "size_t", "size_t"}, "stddef.h"),
    makeLibCPrototype("__strcpy_chk", "char *",
                      {"char *", "const char *", "size_t"}, "stddef.h"),
    makeLibCPrototype("__stpcpy_chk", "char *",
                      {"char *", "const char *", "size_t"}, "stddef.h"),
    makeLibCPrototype("__strncpy_chk", "char *",
                      {"char *", "const char *", "size_t", "size_t"},
                      "stddef.h"),
    makeLibCPrototype("__stpncpy_chk", "char *",
                      {"char *", "const char *", "size_t", "size_t"},
                      "stddef.h"),
    makeLibCPrototype("__strcat_chk", "char *",
                      {"char *", "const char *", "size_t"}, "stddef.h"),
    makeLibCPrototype("__strncat_chk", "char *",
                      {"char *", "const char *", "size_t", "size_t"},
                      "stddef.h"),
    makeLibCPrototype("__gets_chk", "char *", {"char *", "size_t"}, "stddef.h"),
    makeLibCPrototype("__fgets_chk", "char *",
                      {"char *", "size_t", "int", "FILE *"}, "stdio.h"),
    makeLibCPrototype("__fread_chk", "size_t",
                      {"void *", "size_t", "size_t", "size_t", "FILE *"},
                      "stdio.h"),
    makeLibCPrototype("__read_chk", "ssize_t",
                      {"int", "void *", "size_t", "size_t"}, "sys/types.h"),
    makeLibCPrototype("__readlink_chk", "ssize_t",
                      {"const char *", "char *", "size_t", "size_t"},
                      "sys/types.h"),
    makeLibCPrototype("__recv_chk", "ssize_t",
                      {"int", "void *", "size_t", "size_t", "int"},
                      "sys/types.h"),
    makeLibCPrototype("__getcwd_chk", "char *", {"char *", "size_t", "size_t"},
                      "stddef.h"),
    makeLibCPrototype("__realpath_chk", "char *",
                      {"const char *", "char *", "size_t"}, "stddef.h"),
    makeLibCPrototype("__gethostname_chk", "int",
                      {"char *", "size_t", "size_t"}, "stddef.h"),
    makeLibCPrototype("__ttyname_r_chk", "int",
                      {"int", "char *", "size_t", "size_t"}, "stddef.h"),
    makeLibCPrototype("__confstr_chk", "size_t",
                      {"int", "char *", "size_t", "size_t"}, "stddef.h"),
    makeLibCPrototype("__fdelt_chk", "long", {"long"}),
    makeLibCPrototype("__longjmp_chk", "void", {"void *", "int"}),
    makeLibCPrototype("__open_2", "int", {"const char *", "int"}),
    makeLibCPrototype("__open64_2", "int", {"const char *", "int"}),
    makeLibCPrototype("__openat_2", "int", {"int", "const char *", "int"}),
    makeLibCPrototype("__openat64_2", "int", {"int", "const char *", "int"}),
    makeLibCPrototype("__isoc99_scanf", "int", {"const char *", "..."}),
    makeLibCPrototype("__isoc99_fscanf", "int",
                      {"FILE *", "const char *", "..."}, "stdio.h"),
    makeLibCPrototype("__isoc99_sscanf", "int",
                      {"const char *", "const char *", "..."}),
    makeLibCPrototype("__isoc99_vscanf", "int", {"const char *", "void *"}),
    makeLibCPrototype("__isoc99_vfscanf", "int",
                      {"FILE *", "const char *", "void *"}, "stdio.h"),
    makeLibCPrototype("__isoc99_vsscanf", "int",
                      {"const char *", "const char *", "void *"}),
    makeLibCPrototype("__isoc23_scanf", "int", {"const char *", "..."}),
    makeLibCPrototype("__isoc23_fscanf", "int",
                      {"FILE *", "const char *", "..."}, "stdio.h"),
    makeLibCPrototype("__isoc23_sscanf", "int",
                      {"const char *", "const char *", "..."}),
    makeLibCPrototype("__isoc23_vscanf", "int", {"const char *", "void *"}),
    makeLibCPrototype("__isoc23_vfscanf", "int",
                      {"FILE *", "const char *", "void *"}, "stdio.h"),
    makeLibCPrototype("__isoc23_vsscanf", "int",
                      {"const char *", "const char *", "void *"}),
    makeLibCPrototype("__isoc23_strtol", "long",
                      {"const char *", "char **", "int"}),
    makeLibCPrototype("__isoc23_strtoul", "unsigned long",
                      {"const char *", "char **", "int"}),
    makeLibCPrototype("__isoc23_strtoll", "long long",
                      {"const char *", "char **", "int"}),
    makeLibCPrototype("__isoc23_strtoull", "unsigned long long",
                      {"const char *", "char **", "int"}),
    makeLibCPrototype("__isoc23_strtoimax", "intmax_t",
                      {"const char *", "char **", "int"}),
    makeLibCPrototype("__isoc23_strtoumax", "uintmax_t",
                      {"const char *", "char **", "int"}),
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFORTIFY_H
