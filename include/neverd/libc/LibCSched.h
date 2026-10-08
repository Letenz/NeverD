#ifndef NEVERD_LIBC_LIBCSCHED_H
#define NEVERD_LIBC_LIBCSCHED_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sched.h — process scheduling
inline constexpr std::string_view kSchedHeader = "sched.h";

inline constexpr std::array kSchedFunctions = {
    "sched_get_priority_max",
    "sched_get_priority_min",
    "sched_getaffinity",
    "sched_getcpu",
    "sched_getparam",
    "sched_getscheduler",
    "sched_rr_get_interval",
    "sched_setaffinity",
    "sched_setparam",
    "sched_setscheduler",
    "sched_yield",
};

/// Fixed arity of the sched.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSchedArity = std::to_array<LibCArityEntry>({
    {"sched_get_priority_max", {1, 0}},
    {"sched_get_priority_min", {1, 0}},
    {"sched_getaffinity", {3, 0}},
    {"sched_getcpu", {0, 0}},
    {"sched_getparam", {2, 0}},
    {"sched_getscheduler", {1, 0}},
    {"sched_rr_get_interval", {2, 0}},
    {"sched_setaffinity", {3, 0}},
    {"sched_setparam", {2, 0}},
    {"sched_setscheduler", {3, 0}},
    {"sched_yield", {0, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSCHED_H
