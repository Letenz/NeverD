#ifndef NEVERD_LIBC_LIBCSETJMP_H
#define NEVERD_LIBC_LIBCSETJMP_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C setjmp.h — non-local jumps
inline constexpr std::string_view kSetjmpHeader = "setjmp.h";

inline constexpr std::array kSetjmpFunctions = {
    "longjmp",
    "setjmp",
    "siglongjmp",
    "sigsetjmp",
};

/// The setjmp.h functions whose arguments are the same in every C library.
/// setjmp itself is left out: the entry points MSVC's x64 and ARM compilers
/// call take the frame to unwind to as a second argument
/// (kExceptionRuntimeArity).
inline constexpr auto kSetjmpArity = std::to_array<LibCArityEntry>({
    {"longjmp", {2, 0}},
    {"siglongjmp", {2, 0}},
    {"sigsetjmp", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSETJMP_H
