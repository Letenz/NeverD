#ifndef NEVERD_LIBC_LIBCUCONTEXT_H
#define NEVERD_LIBC_LIBCUCONTEXT_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX ucontext.h — user-level context switching
inline constexpr std::string_view kUcontextHeader = "ucontext.h";

inline constexpr std::array kUcontextFunctions = {
    "getcontext",
    "makecontext",
    "setcontext",
    "swapcontext",
};

/// Fixed arity of the ucontext.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kUcontextArity = std::to_array<LibCArityEntry>({
    {"getcontext", {1, 0}},
    {"setcontext", {1, 0}},
    {"swapcontext", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCUCONTEXT_H
