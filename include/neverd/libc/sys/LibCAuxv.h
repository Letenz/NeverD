#ifndef NEVERD_LIBC_SYS_LIBCAUXV_H
#define NEVERD_LIBC_SYS_LIBCAUXV_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// Linux sys/auxv.h — auxiliary vector access
inline constexpr std::string_view kSysAuxvHeader = "sys/auxv.h";

inline constexpr std::array kSysAuxvFunctions = {
    "getauxval",
};

/// Fixed arity of the sys/auxv.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysAuxvArity = std::to_array<LibCArityEntry>({
    {"getauxval", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCAUXV_H
