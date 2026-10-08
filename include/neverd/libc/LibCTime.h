#ifndef NEVERD_LIBC_LIBCTIME_H
#define NEVERD_LIBC_LIBCTIME_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C time.h — calendar time and clocks
inline constexpr std::string_view kTimeHeader = "time.h";

inline constexpr std::array kTimeFunctions = {
    "asctime",    "asctime_r",   "clock",    "clock_gettime", "clock_settime",
    "ctime",      "ctime_r",     "difftime", "gmtime",        "gmtime_r",
    "localtime",  "localtime_r", "mktime",   "nanosleep",     "strftime",
    "strftime_l", "time",        "timegm",   "timespec_get",  "tzset",
};

/// difftime(time_t, time_t) -> double: two integer args, FP return (FpRet).
inline constexpr auto kTimeArity = std::to_array<LibCArityEntry>({
    {"difftime", {2, 0, false, false, true}},
    {"asctime", {1, 0}},
    {"asctime_r", {2, 0}},
    {"clock", {0, 0}},
    {"clock_gettime", {2, 0}},
    {"clock_settime", {2, 0}},
    {"ctime", {1, 0}},
    {"ctime_r", {2, 0}},
    {"gmtime", {1, 0}},
    {"gmtime_r", {2, 0}},
    {"localtime", {1, 0}},
    {"localtime_r", {2, 0}},
    {"mktime", {1, 0}},
    {"nanosleep", {2, 0}},
    {"strftime", {4, 0}},
    {"strftime_l", {5, 0}},
    {"time", {1, 0}},
    {"timespec_get", {2, 0}},
    {"timegm", {1, 0}},
    {"tzset", {0, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCTIME_H
