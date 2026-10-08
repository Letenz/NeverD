#ifndef NEVERD_LIBC_SYS_LIBCUTSNAME_H
#define NEVERD_LIBC_SYS_LIBCUTSNAME_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/utsname.h — system identification
inline constexpr std::string_view kSysUtsnameHeader = "sys/utsname.h";

inline constexpr std::array kSysUtsnameFunctions = {
    "uname",
};

/// Fixed arity of the sys/utsname.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysUtsnameArity = std::to_array<LibCArityEntry>({
    {"uname", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCUTSNAME_H
