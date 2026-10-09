// The printf family: routines whose format names the arguments after their
// fixed parameters, the last of which is the format.  Included with
// LIBC_PRINTF_FORMAT(Name, Return, FixedParameters...), each a C type.  The
// _FORTIFY_SOURCE forms put their flag and object size before the format.

LIBC_PRINTF_FORMAT("printf", "int", "const char *")
LIBC_PRINTF_FORMAT("fprintf", "int", "FILE *", "const char *")
LIBC_PRINTF_FORMAT("dprintf", "int", "int", "const char *")
LIBC_PRINTF_FORMAT("sprintf", "int", "char *", "const char *")
LIBC_PRINTF_FORMAT("snprintf", "int", "char *", "size_t", "const char *")
LIBC_PRINTF_FORMAT("asprintf", "int", "char **", "const char *")
LIBC_PRINTF_FORMAT("syslog", "void", "int", "const char *")
LIBC_PRINTF_FORMAT("err", "void", "int", "const char *")
LIBC_PRINTF_FORMAT("errx", "void", "int", "const char *")
LIBC_PRINTF_FORMAT("warn", "void", "const char *")
LIBC_PRINTF_FORMAT("warnx", "void", "const char *")
LIBC_PRINTF_FORMAT("error", "void", "int", "int", "const char *")
LIBC_PRINTF_FORMAT("__printf_chk", "int", "int", "const char *")
LIBC_PRINTF_FORMAT("__fprintf_chk", "int", "FILE *", "int", "const char *")
LIBC_PRINTF_FORMAT("__dprintf_chk", "int", "int", "int", "const char *")
LIBC_PRINTF_FORMAT("__sprintf_chk", "int", "char *", "int", "size_t",
                   "const char *")
LIBC_PRINTF_FORMAT("__snprintf_chk", "int", "char *", "size_t", "int", "size_t",
                   "const char *")
LIBC_PRINTF_FORMAT("__asprintf_chk", "int", "char **", "int", "const char *")
LIBC_PRINTF_FORMAT("__syslog_chk", "void", "int", "int", "const char *")
