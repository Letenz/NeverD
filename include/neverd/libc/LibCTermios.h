#ifndef NEVERD_LIBC_LIBCTERMIOS_H
#define NEVERD_LIBC_LIBCTERMIOS_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX termios.h — terminal I/O control
inline constexpr std::string_view kTermiosHeader = "termios.h";

inline constexpr std::array kTermiosFunctions = {
    "cfgetispeed", "cfgetospeed", "cfsetispeed", "cfsetospeed",
    "tcdrain",     "tcflow",      "tcflush",     "tcgetattr",
    "tcgetsid",    "tcsendbreak", "tcsetattr",
};

/// Fixed arity of the termios.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kTermiosArity = std::to_array<LibCArityEntry>({
    {"cfgetispeed", {1, 0}},
    {"cfgetospeed", {1, 0}},
    {"cfsetispeed", {2, 0}},
    {"cfsetospeed", {2, 0}},
    {"tcdrain", {1, 0}},
    {"tcflow", {2, 0}},
    {"tcflush", {2, 0}},
    {"tcgetattr", {2, 0}},
    {"tcgetsid", {1, 0}},
    {"tcsendbreak", {2, 0}},
    {"tcsetattr", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCTERMIOS_H
