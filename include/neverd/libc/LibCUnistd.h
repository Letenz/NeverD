#ifndef NEVERD_LIBC_LIBCUNISTD_H
#define NEVERD_LIBC_LIBCUNISTD_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX unistd.h — very common in lifted binaries
inline constexpr std::string_view kUnistdHeader = "unistd.h";

inline constexpr std::array kUnistdFunctions = {
    "_exit",     "access",      "alarm",      "chdir",     "chown",
    "close",     "confstr",     "dup",        "dup2",      "dup3",
    "execl",     "execle",      "execlp",     "execv",     "execve",
    "execvp",    "faccessat",   "fchdir",     "fchown",    "fchownat",
    "fdatasync", "fork",        "fpathconf",  "fsync",     "ftruncate",
    "getcwd",    "getegid",     "getentropy", "geteuid",   "getgid",
    "getgroups", "gethostname", "getlogin",   "getopt",    "getpagesize",
    "getpgid",   "getpgrp",     "getpid",     "getppid",   "getsid",
    "gettid",    "getuid",      "isatty",     "link",      "linkat",
    "lseek",     "lseek64",     "pathconf",   "pipe",      "pipe2",
    "pread",     "pwrite",      "read",       "readlink",  "readlinkat",
    "rmdir",     "setgid",      "setpgid",    "setsid",    "setuid",
    "sleep",     "swab",        "symlink",    "symlinkat", "sysconf",
    "truncate",  "unlink",      "unlinkat",   "usleep",    "write",
};

/// Fixed arity of the non-variadic unistd.h functions (open/fcntl are variadic
/// and are handled by varArgFixedCount instead).  {IntArgs, FpArgs}.
inline constexpr auto kUnistdArity = std::to_array<LibCArityEntry>({
    {"read", {3, 0}},       {"write", {3, 0}},       {"close", {1, 0}},
    {"lseek", {3, 0}},      {"unlink", {1, 0}},      {"access", {2, 0}},
    {"dup", {1, 0}},        {"dup2", {2, 0}},        {"pipe", {1, 0}},
    {"getpid", {0, 0}},     {"getppid", {0, 0}},     {"sleep", {1, 0}},
    {"usleep", {1, 0}},     {"isatty", {1, 0}},      {"chdir", {1, 0}},
    {"rmdir", {1, 0}},      {"fsync", {1, 0}},       {"_exit", {1, 0}},
    {"alarm", {1, 0}},      {"chown", {3, 0}},       {"confstr", {3, 0}},
    {"dup3", {3, 0}},       {"execv", {2, 0}},       {"execve", {3, 0}},
    {"execvp", {2, 0}},     {"faccessat", {4, 0}},   {"fchdir", {1, 0}},
    {"fchown", {3, 0}},     {"fork", {0, 0}},        {"fpathconf", {2, 0}},
    {"ftruncate", {2, 0}},  {"getcwd", {2, 0}},      {"getegid", {0, 0}},
    {"getentropy", {2, 0}}, {"geteuid", {0, 0}},     {"getgid", {0, 0}},
    {"getgroups", {2, 0}},  {"gethostname", {2, 0}}, {"getlogin", {0, 0}},
    {"getopt", {3, 0}},     {"getpagesize", {0, 0}}, {"getpgid", {1, 0}},
    {"getpgrp", {0, 0}},    {"getsid", {1, 0}},      {"gettid", {0, 0}},
    {"getuid", {0, 0}},     {"link", {2, 0}},        {"linkat", {5, 0}},
    {"pathconf", {2, 0}},   {"pipe2", {2, 0}},       {"pread", {4, 0}},
    {"pwrite", {4, 0}},     {"readlink", {3, 0}},    {"readlinkat", {4, 0}},
    {"setgid", {1, 0}},     {"setpgid", {2, 0}},     {"setsid", {0, 0}},
    {"setuid", {1, 0}},     {"swab", {3, 0}},        {"symlink", {2, 0}},
    {"symlinkat", {3, 0}},  {"sysconf", {1, 0}},     {"truncate", {2, 0}},
    {"unlinkat", {3, 0}},   {"fchownat", {5, 0}},    {"fdatasync", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCUNISTD_H
