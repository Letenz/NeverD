#ifndef NEVERD_LIBC_SYS_LIBCPTRACE_H
#define NEVERD_LIBC_SYS_LIBCPTRACE_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// Linux sys/ptrace.h — process tracing
inline constexpr std::string_view kSysPtraceHeader = "sys/ptrace.h";

inline constexpr std::array kSysPtraceFunctions = {
    "ptrace",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCPTRACE_H
