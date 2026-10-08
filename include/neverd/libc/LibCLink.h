#ifndef NEVERD_LIBC_LIBCLINK_H
#define NEVERD_LIBC_LIBCLINK_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// GNU/POSIX link.h — ELF program header iteration
inline constexpr std::string_view kLinkHeader = "link.h";

inline constexpr std::array kLinkFunctions = {
    "dl_iterate_phdr",
};

/// Fixed arity of the link.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kLinkArity = std::to_array<LibCArityEntry>({
    {"dl_iterate_phdr", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCLINK_H
