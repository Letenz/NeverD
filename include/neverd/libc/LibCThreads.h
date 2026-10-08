#ifndef NEVERD_LIBC_LIBCTHREADS_H
#define NEVERD_LIBC_LIBCTHREADS_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// C11 threads.h — threading primitives
inline constexpr std::string_view kThreadsHeader = "threads.h";

inline constexpr std::array kThreadsFunctions = {
    "call_once",   "cnd_broadcast", "cnd_destroy", "cnd_init",
    "cnd_signal",  "cnd_timedwait", "cnd_wait",    "mtx_destroy",
    "mtx_init",    "mtx_lock",      "mtx_trylock", "mtx_unlock",
    "thrd_create", "thrd_current",  "thrd_detach", "thrd_equal",
    "thrd_exit",   "thrd_join",     "tss_create",  "tss_delete",
    "tss_get",     "tss_set",
};

/// Fixed arity of the threads.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kThreadsArity = std::to_array<LibCArityEntry>({
    {"call_once", {2, 0}},   {"cnd_broadcast", {1, 0}},
    {"cnd_destroy", {1, 0}}, {"cnd_init", {1, 0}},
    {"cnd_signal", {1, 0}},  {"cnd_timedwait", {3, 0}},
    {"cnd_wait", {2, 0}},    {"mtx_destroy", {1, 0}},
    {"mtx_init", {2, 0}},    {"mtx_lock", {1, 0}},
    {"mtx_trylock", {1, 0}}, {"mtx_unlock", {1, 0}},
    {"thrd_create", {3, 0}}, {"thrd_current", {0, 0}},
    {"thrd_detach", {1, 0}}, {"thrd_equal", {2, 0}},
    {"thrd_exit", {1, 0}},   {"thrd_join", {2, 0}},
    {"tss_create", {2, 0}},  {"tss_delete", {1, 0}},
    {"tss_get", {1, 0}},     {"tss_set", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCTHREADS_H
