#ifndef NEVERD_LIBC_SYS_LIBCSENDFILE_H
#define NEVERD_LIBC_SYS_LIBCSENDFILE_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// Linux sys/sendfile.h — zero-copy data transfer
inline constexpr std::string_view kSysSendfileHeader = "sys/sendfile.h";

inline constexpr std::array kSysSendfileFunctions = {
    "sendfile",
};

/// Fixed arity of the sys/sendfile.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSysSendfileArity = std::to_array<LibCArityEntry>({
    {"sendfile", {4, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSENDFILE_H
