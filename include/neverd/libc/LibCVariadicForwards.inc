// How C writes a function that passes every argument it receives on to a
// variadic routine, as a stub that jumps to the routine does.  C passes no
// `...` on, so the function hands the routine's variadic arguments to a form
// of it that takes them as one value.  Header declares the types the fixed
// parameters name, or is empty.
//
// LIBC_VA_LIST_FORM(Name, Return, Header, VaListName, Fixed...): VaListName
// takes the variadic arguments as a va_list after the same fixed parameters.
// The last fixed parameter is the format.
//
// LIBC_BOUNDED_VARARGS(Name, Return, Header, Count, Type, Fixed...): the
// routine reads at most Count variadic arguments, each passed as Type, so
// they can be passed on one by one.
//
// LIBC_SENTINEL_VARARGS(Name, Return, Header, VectorName, Environment,
// Fixed...): the variadic arguments are pointers up to a null one, which
// VectorName takes as an array after the first fixed parameter; when
// Environment, an environment array follows the null pointer.

#ifdef LIBC_VA_LIST_FORM
LIBC_VA_LIST_FORM("printf", "int", "", "vprintf", "const char *")
LIBC_VA_LIST_FORM("fprintf", "int", "stdio.h", "vfprintf", "FILE *",
                  "const char *")
LIBC_VA_LIST_FORM("dprintf", "int", "", "vdprintf", "int", "const char *")
LIBC_VA_LIST_FORM("sprintf", "int", "", "vsprintf", "char *", "const char *")
LIBC_VA_LIST_FORM("snprintf", "int", "stddef.h", "vsnprintf", "char *",
                  "size_t", "const char *")
LIBC_VA_LIST_FORM("asprintf", "int", "", "vasprintf", "char **", "const char *")
LIBC_VA_LIST_FORM("syslog", "void", "", "vsyslog", "int", "const char *")
LIBC_VA_LIST_FORM("err", "void", "", "verr", "int", "const char *")
LIBC_VA_LIST_FORM("errx", "void", "", "verrx", "int", "const char *")
LIBC_VA_LIST_FORM("warn", "void", "", "vwarn", "const char *")
LIBC_VA_LIST_FORM("warnx", "void", "", "vwarnx", "const char *")
LIBC_VA_LIST_FORM("scanf", "int", "", "vscanf", "const char *")
LIBC_VA_LIST_FORM("fscanf", "int", "stdio.h", "vfscanf", "FILE *",
                  "const char *")
LIBC_VA_LIST_FORM("sscanf", "int", "", "vsscanf", "const char *",
                  "const char *")
LIBC_VA_LIST_FORM("__isoc99_scanf", "int", "", "__isoc99_vscanf",
                  "const char *")
LIBC_VA_LIST_FORM("__isoc99_fscanf", "int", "stdio.h", "__isoc99_vfscanf",
                  "FILE *", "const char *")
LIBC_VA_LIST_FORM("__isoc99_sscanf", "int", "", "__isoc99_vsscanf",
                  "const char *", "const char *")
LIBC_VA_LIST_FORM("__isoc23_scanf", "int", "", "__isoc23_vscanf",
                  "const char *")
LIBC_VA_LIST_FORM("__isoc23_fscanf", "int", "stdio.h", "__isoc23_vfscanf",
                  "FILE *", "const char *")
LIBC_VA_LIST_FORM("__isoc23_sscanf", "int", "", "__isoc23_vsscanf",
                  "const char *", "const char *")
LIBC_VA_LIST_FORM("__printf_chk", "int", "", "__vprintf_chk", "int",
                  "const char *")
LIBC_VA_LIST_FORM("__fprintf_chk", "int", "stdio.h", "__vfprintf_chk", "FILE *",
                  "int", "const char *")
LIBC_VA_LIST_FORM("__dprintf_chk", "int", "", "__vdprintf_chk", "int", "int",
                  "const char *")
LIBC_VA_LIST_FORM("__sprintf_chk", "int", "stddef.h", "__vsprintf_chk",
                  "char *", "int", "size_t", "const char *")
LIBC_VA_LIST_FORM("__snprintf_chk", "int", "stddef.h", "__vsnprintf_chk",
                  "char *", "size_t", "int", "size_t", "const char *")
LIBC_VA_LIST_FORM("__asprintf_chk", "int", "", "__vasprintf_chk", "char **",
                  "int", "const char *")
LIBC_VA_LIST_FORM("__syslog_chk", "void", "", "__vsyslog_chk", "int", "int",
                  "const char *")
LIBC_VA_LIST_FORM("wprintf", "int", "wchar.h", "vwprintf", "const wchar_t *")
LIBC_VA_LIST_FORM("fwprintf", "int", "wchar.h", "vfwprintf", "FILE *",
                  "const wchar_t *")
LIBC_VA_LIST_FORM("swprintf", "int", "wchar.h", "vswprintf", "wchar_t *",
                  "size_t", "const wchar_t *")
LIBC_VA_LIST_FORM("wscanf", "int", "wchar.h", "vwscanf", "const wchar_t *")
LIBC_VA_LIST_FORM("fwscanf", "int", "wchar.h", "vfwscanf", "FILE *",
                  "const wchar_t *")
LIBC_VA_LIST_FORM("swscanf", "int", "wchar.h", "vswscanf", "const wchar_t *",
                  "const wchar_t *")
LIBC_VA_LIST_FORM("_snprintf", "int", "stddef.h", "_vsnprintf", "char *",
                  "size_t", "const char *")
LIBC_VA_LIST_FORM("_snwprintf", "int", "wchar.h", "_vsnwprintf", "wchar_t *",
                  "size_t", "const wchar_t *")
LIBC_VA_LIST_FORM("_scprintf", "int", "", "_vscprintf", "const char *")
LIBC_VA_LIST_FORM("_scwprintf", "int", "wchar.h", "_vscwprintf",
                  "const wchar_t *")
#undef LIBC_VA_LIST_FORM
#endif

#ifdef LIBC_BOUNDED_VARARGS
LIBC_BOUNDED_VARARGS("open", "int", "", 1, "int", "const char *", "int")
LIBC_BOUNDED_VARARGS("open64", "int", "", 1, "int", "const char *", "int")
LIBC_BOUNDED_VARARGS("openat", "int", "", 1, "int", "int", "const char *",
                     "int")
LIBC_BOUNDED_VARARGS("openat64", "int", "", 1, "int", "int", "const char *",
                     "int")
LIBC_BOUNDED_VARARGS("fcntl", "int", "", 1, "void *", "int", "int")
LIBC_BOUNDED_VARARGS("fcntl64", "int", "", 1, "void *", "int", "int")
LIBC_BOUNDED_VARARGS("ioctl", "int", "", 1, "void *", "int", "unsigned long")
LIBC_BOUNDED_VARARGS("prctl", "int", "", 4, "unsigned long", "int")
LIBC_BOUNDED_VARARGS("syscall", "long", "", 6, "long", "long")
LIBC_BOUNDED_VARARGS("ulimit", "long", "", 1, "long", "int")
#undef LIBC_BOUNDED_VARARGS
#endif

#ifdef LIBC_SENTINEL_VARARGS
LIBC_SENTINEL_VARARGS("execl", "int", "", "execv", false, "const char *",
                      "const char *")
LIBC_SENTINEL_VARARGS("execlp", "int", "", "execvp", false, "const char *",
                      "const char *")
LIBC_SENTINEL_VARARGS("execle", "int", "", "execve", true, "const char *",
                      "const char *")
#undef LIBC_SENTINEL_VARARGS
#endif
