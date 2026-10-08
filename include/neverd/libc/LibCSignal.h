#ifndef NEVERD_LIBC_LIBCSIGNAL_H
#define NEVERD_LIBC_LIBCSIGNAL_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C signal.h — signal handling
inline constexpr std::string_view kSignalHeader = "signal.h";

inline constexpr std::array kSignalFunctions = {
    "kill",        "raise",       "sigaction",       "sigaddset",
    "sigaltstack", "sigdelset",   "sigemptyset",     "sigfillset",
    "signal",      "sigprocmask", "pthread_sigmask", "sigismember",
};

/// Fixed arity of the signal.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSignalArity = std::to_array<LibCArityEntry>({
    {"kill", {2, 0}},
    {"raise", {1, 0}},
    {"sigaction", {3, 0}},
    {"sigaddset", {2, 0}},
    {"sigaltstack", {2, 0}},
    {"sigdelset", {2, 0}},
    {"sigemptyset", {1, 0}},
    {"sigfillset", {1, 0}},
    {"signal", {2, 0}},
    {"sigprocmask", {3, 0}},
    {"pthread_sigmask", {3, 0}},
    {"sigismember", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSIGNAL_H
